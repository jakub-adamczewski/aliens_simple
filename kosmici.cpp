#include <mpi.h>
#include <pthread.h>
#include <cstdio>
#include <vector>
#include <ctime>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <mutex>
#include <algorithm>
#include <condition_variable>
#include <string>

// Params to define
#define PURPLES_COUNT 2
#define BLUES_COUNT 2
const int HOTEL_CAPACITIES[] = {1, 2};


#define ALL_ALIENS_COUNT (PURPLES_COUNT + BLUES_COUNT)

#define FIRST_ID 0
#define LAST_ID (ALL_ALIENS_COUNT-1)
#define PURPLES_FIRST_ID FIRST_ID
#define PURPLES_LAST_ID (PURPLES_COUNT - 1)
#define BLUES_FIRST_ID PURPLES_COUNT
#define BLUES_LAST_ID LAST_ID


const int HOTELS_NUMBER = sizeof(HOTEL_CAPACITIES) / sizeof(int);

#define MIN_SLEEP 1
#define MAX_SLEEP 4

enum AlienType {
    PURPLE,
    BLUE
};

enum MessageType {
    HOTEL_REQUEST,
    HOTEL_REQUEST_RESP, // sent only when alien has not been in any hotel yet
    HOTEL_RELEASE
};

enum ProcessStatus {
    NO_HOTEL,
    WAITING_TO_ENTER_HOTEL,
    IN_HOTEL
};

struct packet {
    int clock;
    int alienId;
    int hotelId;
};

bool operator==(const packet &a, const packet &b) {
    return a.clock == b.clock &&
           a.alienId == b.alienId &&
           a.hotelId == b.hotelId &&
}

struct past_request {
    packet msg;
    bool leftHotel;
};

#define debug(FORMAT, ...) printf("\033[c:%d][r:%d][%c][%s][%s]: " FORMAT "\033[0m\n", this->clock, this->rank, (this->alienType == PURPLE ? 'P' : 'B'), toString(this->processStatus),printTime(), ##__VA_ARGS__);

char *printTime() {
    time_t rawTime;
    struct tm *timeInfo;
    char *buffer = new char[10];

    time(&rawTime);
    timeInfo = localtime(&rawTime);
    strftime(buffer, 10, "%H:%M:%S", timeInfo);
    return buffer;
}

static char *toString(ProcessStatus status) {
    switch (status) {
        case NO_HOTEL:
            return "NO_HOTEL";
        case WAITING_TO_ENTER_HOTEL:
            return "WAITING_TO_ENTER_HOTEL";
        case IN_HOTEL:
            return "IN_HOTEL";
    }
}

static AlienType getAlienType(int alienId) {
    assert(alienId >= FIRST_ID && alienId <= LAST_ID);
    if (alienId >= PURPLES_FIRST_ID && alienId <= PURPLES_LAST_ID) {
        return PURPLE;
    } else if (alienId >= BLUES_FIRST_ID && alienId <= BLUES_LAST_ID) {
        return BLUE;
    }
}

static AlienType getOtherFraction(AlienType tp) {
    switch (tp) {
        case PURPLE:
            return BLUE;
        case BLUE:
            return PURPLE;
    }
}

static int getFractionCount(AlienType tp) {
    switch (tp) {
        case PURPLE:
            return PURPLES_COUNT;
        case BLUE:
            return BLUES_COUNT;
    }
}

static int randInt(int _min, int _max) {
    return _min + (rand() % (_max - _min));
}

static int getRandomHotelId() {
    return randInt(0, HOTELS_NUMBER + 1);
}

class Entity {
public:
    int clock;
    pthread_mutex_t clock_mutex;
    int rank;

    virtual void main() = 0;

    virtual void communication() = 0;

    Entity(int c, pthread_mutex_t m, int r) {
        this->clock = c;
        this->clock_mutex = m;
        this->rank = r;
    }

    int incrementAndGetClock() {
        pthread_mutex_lock(&this->clock_mutex);
        int clock_value = ++this->clock;
        pthread_mutex_unlock(&this->clock_mutex);
        return clock_value;
    }

    int getClock() {
        pthread_mutex_lock(&this->clock_mutex);
        int clock_value = this->clock;
        pthread_mutex_unlock(&this->clock_mutex);
        return clock_value;
    }

    int updateAndIncrementClock(int c) {
        pthread_mutex_lock(&this->clock_mutex);
        if (c > this->clock) {
            this->clock = c + 1;
        } else {
            this->clock += 1;
        }
        c = this->clock;
        pthread_mutex_unlock(&this->clock_mutex);
        return c;
    }

    int updateClock(int c) {
        pthread_mutex_lock(&this->clock_mutex);
        if (c > this->clock) {
            this->clock = c;
        }
        pthread_mutex_unlock(&this->clock_mutex);
        return c;
    }

    static void *runComm(void *e) {
        ((Entity *) e)->communication();
        return nullptr;
    }

    static void threadSleep(int s) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000 * s));
    }
};

class Alien : public Entity {

    AlienType alienType = getAlienType(rank);
    std::condition_variable enterHotelCond;
    std::mutex canEnterPickedHotel;
    pthread_mutex_t msgVectorsMutex = PTHREAD_MUTEX_INITIALIZER;
    std::vector<struct past_request> hotelRequests; // Moze dojsc do sytuacji ze ktos wyjdzie z hotelu w trakcie jak my do niego czekamy.
    std::vector<struct packet> hotelResponses;
    ProcessStatus processStatus = NO_HOTEL;
    bool notPickedAnyHotelYet = true;

    struct packet myHotelRequest;

    void sendHotelRequest(int pickedHotelId) {
        int clk = this->incrementAndGetClock();
        struct packet msg =
                {
                        clk,
                        this->rank,
                        pickedHotelId
                };
        sendMsgToAllOtherAliens(msg, HOTEL_REQUEST);

        myHotelRequest = msg;
        processStatus = WAITING_TO_ENTER_HOTEL;
        notPickedAnyHotelYet = false;

        debug("Sent hotel requests for hotel %d.", pickedHotelId);
    }

    void sendHotelResponse(int alienTo) {
        struct packet msg =
                {
                        getClock(),
                        this->rank
                };
        MPI_Send(&msg, sizeof(msg), MPI_BYTE, alienTo, HOTEL_REQUEST_RESP, MPI_COMM_WORLD);
    }

    void enterHotelForRandomTime() {
        debug("Entered hotel %d", myHotelRequest.hotelId)
        processStatus = IN_HOTEL;
        this->randomSleep();

        processStatus = NO_HOTEL;
        struct packet emptyMsg = {
                NULL, NULL, NULL
        };
        myHotelRequest = emptyMsg;

        struct packet msg =
                {
                        this->getClock(),
                        this->rank
                };
        sendMsgToAllOtherAliens(msg, HOTEL_RELEASE);
        debug("Left hotel %d and informed others.", myHotelRequest.hotelId);
    }

    void sendMsgToAllOtherAliens(struct packet msg, MessageType msgType) {
        for (int destID = FIRST_ID; destID <= LAST_ID; destID++) {
            if (destID == rank) continue;
            MPI_Send(&msg, sizeof(msg), MPI_BYTE, destID, msgType, MPI_COMM_WORLD);
        }
    }

    void randomSleep() {
        int r = randInt(MIN_SLEEP, MAX_SLEEP);
        debug("sleeping for %d seconds", r);
        Entity::threadSleep(r);
    }

    void removeOldAlienMessagesAndAddNewHotelRequest(struct packet msg) {
        pthread_mutex_lock(&this->msgVectorsMutex);

        hotelRequests.erase(
                std::remove_if(
                        hotelRequests.begin(),
                        hotelRequests.end(),
                        [](const past_request saved) { return saved.msg.alienId == msg.alienId; }
                ),
                hotelRequests.end()
        );
        hotelResponses.erase(
                std::remove_if(
                        hotelResponses.begin(),
                        hotelResponses.end(),
                        [](const packet saved) { return saved.alientId == msg.alienId; }
                ),
                hotelResponses.end()
        );

        struct past_request req = {
                msg,
                false
        };
        this->hotelRequests.push_back(req);
        this->sortHotelRequestsWithoutMutex();

        pthread_mutex_unlock(&this->msgVectorsMutex);
    }

    void sortHotelRequestsWithoutMutex() {
        sort(
                this->hotelRequests.begin(),
                this->hotelRequests.end(),
                [](const struct past_request &a, const struct past_request &b) {
                    assert(a.msg.clock != b.msg.clock);
                    return a.msg.clock < b.msg.clock;
                }
        );
    }

    void addHotelResponse(struct packet msg) {
        pthread_mutex_lock(&this->msgVectorsMutex);
        this->hotelResponses.push_back(msg);
        pthread_mutex_unlock(&this->msgVectorsMutex);
    }

    void markThatAlienLeftHotel(packet release_msg) {
        pthread_mutex_lock(&this->msgVectorsMutex);
        for (auto &hotelRequest: hotelRequests) {
            if (hotelRequest.msg.alienId == release_msg.alienId &&
                hotelRequest.msg.hotelId == release_msg.hotelId) {
                hotelRequest.leftHotel = true;
            }
        }
        pthread_mutex_unlock(&this->msgVectorsMutex);
    }

    bool checkIfCanEnterHotel(bool notifyCond = true) {

        int myFractionAliensInFrontOfMe = 0;
        int myFractionAliensBehindMeOrNotToMyHotel = 0;
        int otherFractionAliensInFrontOfMe = 0;
        int otherFractionAliensBehindMeOrNotToMyHotel = 0;

        pthread_mutex_lock(&this->msgVectorsMutex);

        for (auto pastRequest: hotelRequests) {
            assert(pastRequest.msg.clock != this->myHotelRequest.clock);

            bool isRequestFromMyFraction = getAlienType(pastRequest.msg.alienId) == this->alienType;
            bool isRequestOlderThanMine = pastRequest.msg.clock < this->myHotelRequest.clock;
            bool isRequestForSameHotel = pastRequest.msg.hotelId == myHotelRequest.hotelId;
            bool isAlienStillInHotel = !pastRequest.leftHotel;

            if (isRequestFromMyFraction) {
                if (isRequestOlderThanMine && isRequestForSameHotel && isAlienStillInHotel) {
                    myFractionAliensInFrontOfMe++;
                } else {
                    myFractionAliensBehindMeOrNotToMyHotel++;
                }
            } else {
                if (isRequestOlderThanMine && isRequestForSameHotel && isAlienStillInHotel) {
                    otherFractionAliensInFrontOfMe++;
                } else {
                    otherFractionAliensBehindMeOrNotToMyHotel++;
                }
            }
        }

        for (auto response: hotelResponses) {
            bool isRequestFromMyFraction = getAlienType(response.alienId) == this->alienType;
            if (isRequestFromMyFraction) {
                myFractionAliensBehindMeOrNotToMyHotel++;
            } else {
                otherFractionAliensBehindMeOrNotToMyHotel++;
            }
        }

        pthread_mutex_unlock(&this->msgVectorsMutex);


        bool noOtherFractionAliensInFrontOfMe = otherFractionAliensInFrontOfMe == 0;
        bool weHaveInfoAboutAllOtherFractionAliens =
                (otherFractionAliensInFrontOfMe + otherFractionAliensBehindMeOrNotToMyHotel) ==
                getFractionCount(getOtherFraction(alienType));
        bool isAnyPlaceForMeInHotel = myFractionAliensInFrontOfMe + 1 <= HOTEL_CAPACITIES[myHotelRequest.hotelId];

        bool canEnter = noOtherFractionAliensInFrontOfMe &&
                        weHaveInfoAboutAllOtherFractionAliens &&
                        isAnyPlaceForMeInHotel;

        if (canEnter && notifyCond) {
            this->enterHotelCond.notify_all();
        }
        return canEnter;
    }

public:
    Alien(int c, pthread_mutex_t m, int r) : Entity(c, m, r) {
//        std::fill_n(hotelBookings, ALL_ALIENS_COUNT, -1);
    }

    void main() override {
        std::unique_lock<std::mutex> lck(this->canEnterPickedHotel);
        while (true) {
            debug("Looking for hotel.");
            this->randomSleep();
            this->sendHotelRequest(getRandomHotelId());
            bool canEnterHotel = checkIfCanEnterHotel(false);
            if (!canEnterHotel) {
                this->enterHotelCond.wait(lck);
            }
            this->enterHotelForRandomTime();
        }
    }

    void communication() override {
        struct packet msg;
        MPI_Status status;
        while (true) {
            MPI_Recv(&msg, sizeof(msg), MPI_BYTE, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);

            updateClock(msg.clock);
            switch ((MessageType) status.MPI_TAG) {
                case HOTEL_REQUEST:
                    debug("Got HOTEL_REQUEST");
                    removeOldAlienMessagesAndAddNewHotelRequest(msg);
                    if (notPickedAnyHotelYet) {
                        sendHotelResponse(msg.alienId);
                    }
                    break;
                case HOTEL_RELEASE:
                    debug("Got HOTEL_RELEASE");
                    markThatAlienLeftHotel(msg);
                    break;
                case HOTEL_REQUEST_RESP:
                    debug("Got HOTEL_REQUEST_RESP");
                    break;
            }
            if (processStatus == WAITING_TO_ENTER_HOTEL) {
                checkIfCanEnterHotel();
            }
        }
    }
};


int main(int argc, char **argv) {
    int clock = 0;
    pthread_mutex_t clockMutex = PTHREAD_MUTEX_INITIALIZER;
    int rank;
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == 0) {
        // Print configuration
        int world_size;

        MPI_Comm_size(MPI_COMM_WORLD, &world_size);
        printf("Purple aliens: %d\n", PURPLES_COUNT);
        printf("Blue aliens: %d\n", BLUES_COUNT);
        printf("Hotels: %d\n", HOTELS_NUMBER);

        printf("Number of processes running: %d\n\n", world_size);
        if (world_size != ALL_ALIENS_COUNT) {
            printf("Wrong processes amount.\n");
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
        if (PURPLES_COUNT <= 0 || BLUES_COUNT <= 0 || HOTELS_NUMBER <= 0) {
            printf("There has to be at least one of every process type and hotels.\n");
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
    }

    Entity *entity;
    if (rank >= FIRST_ID && rank <= LAST_ID) {
        entity = new Alien(clock, clockMutex, rank);
    } else {
        printf("Wrong alienId: %d\n", rank);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    pthread_t thd;
    pthread_create(&thd, nullptr, &Entity::runComm, entity);
    entity->main();

    MPI_Finalize();
}