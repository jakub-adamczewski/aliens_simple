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
#define MAX_SLEEP 3

enum AlienType {
    PURPLE,
    BLUE
};

enum MessageType {
    HOTEL_REQUEST,
    HOTEL_REQUEST_ACK, // sent only when alien has not been in any hotel yet
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
           a.hotelId == b.hotelId;
}

struct past_request {
    packet msg;
    bool leftHotel;
};

#define debug(FORMAT, ...) printf("\033 |c:%d|r:%d|f:%c|s:%s|t:%s|msg:" FORMAT "|\n", this->clock, this->rank, (this->alienType == PURPLE ? 'P' : 'B'), toString(this->processStatus),printTime(),##__VA_ARGS__);

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

        srand(time(nullptr) + this->rank);
    }

    int incrementAndGetClock() {
        pthread_mutex_lock(&this->clock_mutex);
        int clock_value = ++this->clock;
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

    static void *runComm(void *e) {
        ((Entity *) e)->communication();
        return nullptr;
    }

    static void threadSleep(int s) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000 * s));
    }
};

class Alien : public Entity {

    AlienType alienType = Alien::getAlienType(rank);
    std::condition_variable enterHotelCond;
    std::mutex canEnterPickedHotel;
    pthread_mutex_t msgVectorsMutex = PTHREAD_MUTEX_INITIALIZER;
    std::vector<struct past_request> hotelRequests;
    ProcessStatus processStatus = NO_HOTEL;
    pthread_mutex_t counterMutex = PTHREAD_MUTEX_INITIALIZER;
    int ackCounter = 0;

    struct packet myHotelRequest;

    static AlienType getAlienType(int alienId) {
        if (alienId >= PURPLES_FIRST_ID && alienId <= PURPLES_LAST_ID) {
            return PURPLE;
        } else if (alienId >= BLUES_FIRST_ID && alienId <= BLUES_LAST_ID) {
            return BLUE;
        }
    }

    void incrementAckCounter() {
        pthread_mutex_lock(&this->counterMutex);
        ackCounter++;
        pthread_mutex_unlock(&this->counterMutex);
    }

    void resetAckCounter() {
        pthread_mutex_lock(&this->counterMutex);
        ackCounter = 0;
        pthread_mutex_unlock(&this->counterMutex);
    }

    int getAckCounter() {
        int c;
        pthread_mutex_lock(&this->counterMutex);
        c = ackCounter;
        pthread_mutex_unlock(&this->counterMutex);
        return c;
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

    int randIntInclusive(int _min, int _max) {
        return _min + (rand() % (_max - _min + 1));
    }

    int getRandomHotelId() {
        return randIntInclusive(0, HOTELS_NUMBER - 1);
    }

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

        debug("Sent requests for hotel %d.", pickedHotelId);
    }

    void sendHotelAck(int alienTo) {
        struct packet msg =
                {
                        incrementAndGetClock(),
                        this->rank
                };
        MPI_Send(&msg, sizeof(msg), MPI_BYTE, alienTo, HOTEL_REQUEST_ACK, MPI_COMM_WORLD);
    }

    void enterHotelForRandomTime() {
        processStatus = IN_HOTEL;
        debug("E%d.marker", myHotelRequest.hotelId); // entered hotel %d
        this->randomSleep();

        processStatus = NO_HOTEL;

        struct packet msg =
                {
                        this->incrementAndGetClock(),
                        this->rank
                };
        sendMsgToAllOtherAliens(msg, HOTEL_RELEASE);

        debug("L%d.marker", myHotelRequest.hotelId); // left hotel %d
        struct packet emptyMsg = {
                -1, -1, -1
        };
        myHotelRequest = emptyMsg;
        resetAckCounter();
    }

    void sendMsgToAllOtherAliens(struct packet msg, MessageType msgType) {
        for (int destID = FIRST_ID; destID <= LAST_ID; destID++) {
            if (destID == rank) continue;
            MPI_Send(&msg, sizeof(msg), MPI_BYTE, destID, msgType, MPI_COMM_WORLD);
        }
    }

    void randomSleep() {
        int r = randIntInclusive(MIN_SLEEP, MAX_SLEEP);
        debug("sleeping for %d seconds", r);
        Entity::threadSleep(r);
    }

    void removeOldAndAddNewRequest(struct packet msg) {
        pthread_mutex_lock(&this->msgVectorsMutex);

        hotelRequests.erase(
                std::remove_if(
                        hotelRequests.begin(),
                        hotelRequests.end(),
                        [msg](const past_request saved) { return saved.msg.alienId == msg.alienId; }
                ),
                hotelRequests.end()
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
                    return a.msg.clock < b.msg.clock;
                }
        );
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
        int myFractionAliensBehindMeOrNotToMyHotelOrLeftHotel = 0;
        int otherFractionAliensInFrontOfMe = 0;
        int otherFractionAliensBehindMeOrNotToMyHotelOrLeftHotel = 0;

        pthread_mutex_lock(&this->msgVectorsMutex);

        for (auto pastRequest: hotelRequests) {

            bool isRequestFromMyFraction = Alien::getAlienType(pastRequest.msg.alienId) == this->alienType;
            bool isRequestOlderThanMine = pastRequest.msg.clock < this->myHotelRequest.clock;
            bool isRequestForSameHotel = pastRequest.msg.hotelId == myHotelRequest.hotelId;
            bool isAlienStillInHotel = !pastRequest.leftHotel;

            if (isRequestFromMyFraction) {
                if (isRequestOlderThanMine && isRequestForSameHotel && isAlienStillInHotel) {
                    myFractionAliensInFrontOfMe++;
                } else {
                    myFractionAliensBehindMeOrNotToMyHotelOrLeftHotel++;
                }
            } else {
                if (isRequestOlderThanMine && isRequestForSameHotel && isAlienStillInHotel) {
                    otherFractionAliensInFrontOfMe++;
                } else {
                    otherFractionAliensBehindMeOrNotToMyHotelOrLeftHotel++;
                }
            }
        }

        pthread_mutex_unlock(&this->msgVectorsMutex);


        bool noOtherFractionAliensInFrontOfMe = otherFractionAliensInFrontOfMe == 0;
        bool weHaveInfoAboutAllOtherFractionAliens =
                (otherFractionAliensInFrontOfMe + otherFractionAliensBehindMeOrNotToMyHotelOrLeftHotel) ==
                Alien::getFractionCount(Alien::getOtherFraction(alienType));
        bool isAnyPlaceForMeInHotel = myFractionAliensInFrontOfMe + 1 <= HOTEL_CAPACITIES[myHotelRequest.hotelId];
        bool gotAllAck = getAckCounter() == ALL_ALIENS_COUNT - 1;

        bool canEnter = noOtherFractionAliensInFrontOfMe &&
                        weHaveInfoAboutAllOtherFractionAliens &&
                        isAnyPlaceForMeInHotel &&
                        gotAllAck;

        if (canEnter && notifyCond) {
            this->enterHotelCond.notify_all();
        }
        return canEnter;
    }

public:
    Alien(int c, pthread_mutex_t m, int r) : Entity(c, m, r) {
    }

    void main() override {
        std::unique_lock <std::mutex> lck(this->canEnterPickedHotel);
        while (true) {
            debug("Looking for hotel.");
            this->randomSleep();
            this->sendHotelRequest(Alien::getRandomHotelId());
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

            updateAndIncrementClock(msg.clock);
            switch ((MessageType) status.MPI_TAG) {
                case HOTEL_REQUEST:
                    debug("Got HOTEL_REQUEST");
                    sendHotelAck(msg.alienId);
                    removeOldAndAddNewRequest(msg);
                    break;
                case HOTEL_RELEASE:
                    debug("Got HOTEL_RELEASE");
                    markThatAlienLeftHotel(msg);
                    break;
                case HOTEL_REQUEST_ACK:
                    debug("Got HOTEL_REQUEST_ACK");
                    incrementAckCounter();
                    break;
            }
            if (processStatus == WAITING_TO_ENTER_HOTEL) {
                checkIfCanEnterHotel();
            }
        }
    }
};

void check_thread_support(int provided) {
    printf("THREAD SUPPORT: chcemy %d. Co otrzymamy?\n", provided);
    switch (provided) {
        case MPI_THREAD_SINGLE:
            printf("Brak wsparcia dla wątków, kończę\n");
            /* Nie ma co, trzeba wychodzić */
            fprintf(stderr, "Brak wystarczającego wsparcia dla wątków - wychodzę!\n");
            MPI_Finalize();
            exit(-1);
            break;
        case MPI_THREAD_FUNNELED:
            printf("tylko te wątki, ktore wykonaly mpi_init_thread mogą wykonać wołania do biblioteki mpi\n");
            break;
        case MPI_THREAD_SERIALIZED:
            /* Potrzebne zamki wokół wywołań biblioteki MPI */
            printf("tylko jeden watek naraz może wykonać wołania do biblioteki MPI\n");
            break;
        case MPI_THREAD_MULTIPLE:
            printf("Pełne wsparcie dla wątków\n"); /* tego chcemy. Wszystkie inne powodują problemy */
            break;
        default:
            printf("Nikt nic nie wie\n");
    }
}

int main(int argc, char **argv) {
    int clock = 0;
    pthread_mutex_t clockMutex = PTHREAD_MUTEX_INITIALIZER;
    int rank;
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    check_thread_support(provided);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    // Print configuration
    int world_size;

    if (rank == 0) {
        MPI_Comm_size(MPI_COMM_WORLD, &world_size);
        printf("Purple aliens: %d\n", PURPLES_COUNT);
        printf("Blue aliens: %d\n", BLUES_COUNT);
        printf("Hotels: %d\n", HOTELS_NUMBER);

        printf("Number of processes running: %d\n\n", world_size);
    }

    if (world_size != ALL_ALIENS_COUNT) {
        printf("Wrong processes amount.\n");
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    if (PURPLES_COUNT <= 0 || BLUES_COUNT <= 0 || HOTELS_NUMBER <= 0) {
        printf("There has to be at least one of every process type and hotels.\n");
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    Alien *alien;
    if (rank >= FIRST_ID && rank <= LAST_ID) {
        alien = new Alien(clock, clockMutex, rank);
    } else {
        printf("Wrong alienId: %d\n", rank);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    pthread_t thd;
    pthread_create(&thd, nullptr, &Entity::runComm, alien);
    alien->main();

    MPI_Finalize();
}