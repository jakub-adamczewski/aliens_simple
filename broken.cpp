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
#include <assert.h>

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


const int HOTELS_NUMBER = sizeof(HOTEL_CAPACITIES) / sizeof(HOTEL_CAPACITIES[0]);

#define MIN_SLEEP 2
#define MAX_SLEEP 4

enum AlienType {
    PURPLE,
    BLUE
};

enum MessageType {
    HOTEL_REQUEST,
    HOTEL_REQUEST_ACK,
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

struct past_request {
    packet msg;
    bool leftHotel;
};

#define debug(FORMAT, ...) printf("\033 |c:%d|r:%d|f:%c|s:%s|t:%s|msg:" FORMAT "|\n", this->getClock(), this->rank, toChar(this->alienType), toString(this->processStatus),printTime(),##__VA_ARGS__);

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

static char toChar(AlienType tp) {
    switch (tp) {
        case PURPLE:
            return 'P';
        case BLUE:
            return 'B';
    }
}

class Entity {
    int clocks[ALL_ALIENS_COUNT] = {};
    pthread_mutex_t clocks_mutex = PTHREAD_MUTEX_INITIALIZER;

    void assertProperStartStateOfClocks() {
        pthread_mutex_lock(&this->clocks_mutex);
        for (int c: clocks) {
            assert(c == 0);
        }
        pthread_mutex_unlock(&this->clocks_mutex);
    }

protected:
    int rank;

public:

    virtual void main() = 0;

    virtual void communication() = 0;

    Entity(int r) {
        this->rank = r;
        srand(time(nullptr) + this->rank);
        assertProperStartStateOfClocks();
    }

    int getClock() {
        pthread_mutex_lock(&this->clocks_mutex);
        int clock_value = this->clocks[rank];
        pthread_mutex_unlock(&this->clocks_mutex);
        return clock_value;
    }

    int incrementAndGetClock() {
        pthread_mutex_lock(&this->clocks_mutex);
        int clock_value = ++this->clocks[rank];
        pthread_mutex_unlock(&this->clocks_mutex);
        return clock_value;
    }

    void updateClocks(packet msg) {
        pthread_mutex_lock(&this->clocks_mutex);
        clocks[rank] = std::max(clocks[rank], msg.clock) + 1;
        clocks[msg.alienId] = msg.clock;
        pthread_mutex_unlock(&this->clocks_mutex);
    }

    bool checkIfMyClockIsTheBiggestAndAllClocksInitialized() {
        pthread_mutex_lock(&this->clocks_mutex);
        int myClk = clocks[rank];
        for (int i_rank = 0; i_rank < ALL_ALIENS_COUNT; i_rank++) {
            int i_clk = clocks[i_rank];
            if (i_clk == 0) return false;
            if (i_rank == rank) continue;
            if (i_clk > myClk || (i_clk == myClk && i_rank > rank)) return false;
        }
        pthread_mutex_unlock(&this->clocks_mutex);
        return true;
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
    pthread_mutex_t hotelRequestsMutex = PTHREAD_MUTEX_INITIALIZER;
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

    void sendHotelRequests(int pickedHotelId) {
        struct packet msg =
                {
                        this->incrementAndGetClock(),
                        this->rank,
                        pickedHotelId
                };
        myHotelRequest = msg;
        processStatus = WAITING_TO_ENTER_HOTEL;
        sendMsgToAllOtherAliens(msg, HOTEL_REQUEST);
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

    void removeOldAndAddNewRequest(struct packet newMsg) {
        pthread_mutex_lock(&this->hotelRequestsMutex);

        hotelRequests.erase(
                std::remove_if(
                        hotelRequests.begin(),
                        hotelRequests.end(),
                        [newMsg](const past_request saved) { return saved.msg.alienId == newMsg.alienId; }
                ),
                hotelRequests.end()
        );

        struct past_request req = {
                newMsg,
                false
        };
        this->hotelRequests.push_back(req);
        this->sortHotelRequestsWithoutMutex();

        pthread_mutex_unlock(&this->hotelRequestsMutex);
    }

    void sortHotelRequestsWithoutMutex() {
        sort(
                this->hotelRequests.begin(),
                this->hotelRequests.end(),
                [](const struct past_request &a, const struct past_request &b) {
                    return a.msg.clock < b.msg.clock ||
                           (a.msg.clock == b.msg.clock && a.msg.alienId < b.msg.alienId);
                }
        );
    }

    void markThatAlienLeftHotel(packet release_msg) {
        pthread_mutex_lock(&this->hotelRequestsMutex);
        int edited = 0;
        for (auto &hotelRequest: hotelRequests) {
            if (hotelRequest.msg.alienId == release_msg.alienId) {
                hotelRequest.leftHotel = true;
                edited++;
            }
        }
        assert(edited == 1);
        pthread_mutex_unlock(&this->hotelRequestsMutex);
    }

    static bool packetsCompare(const struct packet a, const struct packet b) {
        return a.clock < b.clock || (a.clock == b.clock && a.alienId < b.alienId);
    }

    bool checkIfCanEnterHotel(bool notifyCond = true) {
        assert(myHotelRequest.clock != -1);
        assert(processStatus == WAITING_TO_ENTER_HOTEL);
        // not in front of me, means:
        // - behind me
        // - not to my hotel
        // - left mine hotel
        int myFractionAliensInFrontOfMe = 0;
        int otherFractionAliensInFrontOfMe = 0;
        int otherFractionAliensNotInFrontOfMe = 0;

        pthread_mutex_lock(&this->hotelRequestsMutex);

        for (auto pastRequest: hotelRequests) {

            bool isRequestFromMyFraction = Alien::getAlienType(pastRequest.msg.alienId) == this->alienType;
            bool isRequestOlderThanMine = Alien::packetsCompare(pastRequest.msg, myHotelRequest);
            bool isRequestForSameHotel = pastRequest.msg.hotelId == myHotelRequest.hotelId;
            bool isAlienStillInHotel = !pastRequest.leftHotel;

            if (isRequestFromMyFraction) {
                if (isRequestOlderThanMine && isRequestForSameHotel && isAlienStillInHotel) {
                    myFractionAliensInFrontOfMe++;
                }
            } else {
                if (isRequestOlderThanMine && isRequestForSameHotel && isAlienStillInHotel) {
                    otherFractionAliensInFrontOfMe++;
                } else {
                    otherFractionAliensNotInFrontOfMe++;
                }
            }
        }

        pthread_mutex_unlock(&this->hotelRequestsMutex);

        bool noOtherFractionAliensInFrontOfMe = otherFractionAliensInFrontOfMe == 0;
        bool weHaveInfoAboutAllOtherFractionAliens =
                (otherFractionAliensInFrontOfMe + otherFractionAliensNotInFrontOfMe) ==
                Alien::getFractionCount(Alien::getOtherFraction(alienType));
        bool isAnyPlaceForMeInHotel = myFractionAliensInFrontOfMe + 1 <= HOTEL_CAPACITIES[myHotelRequest.hotelId];
        bool gotAllAck = getAckCounter() == ALL_ALIENS_COUNT - 1;
        bool isClocksStateCorrect = checkIfMyClockIsTheBiggestAndAllClocksInitialized();

        bool canEnter = noOtherFractionAliensInFrontOfMe &&
                        weHaveInfoAboutAllOtherFractionAliens &&
                        isAnyPlaceForMeInHotel &&
                        gotAllAck &&
                        isClocksStateCorrect;

        if (canEnter && notifyCond) {
            debug(
                    "Can enter hotel noOtherFractionAliensInFrontOfMe %d, weHaveInfoAboutAllOtherFractionAliens %d, isAnyPlaceForMeInHotel %d, gotAllAck %d",
                    noOtherFractionAliensInFrontOfMe,
                    weHaveInfoAboutAllOtherFractionAliens,
                    isAnyPlaceForMeInHotel,
                    gotAllAck
            );
            this->enterHotelCond.notify_all();
        } else {
            debug(
                    "Can not enter hotel noOtherFractionAliensInFrontOfMe %d, weHaveInfoAboutAllOtherFractionAliens %d, isAnyPlaceForMeInHotel %d, gotAllAck %d",
                    noOtherFractionAliensInFrontOfMe,
                    weHaveInfoAboutAllOtherFractionAliens,
                    isAnyPlaceForMeInHotel,
                    gotAllAck
            );
        }
        return canEnter;
    }

public:
    Alien(int r) : Entity(r) {}

    void main() override {
        std::unique_lock <std::mutex> lck(this->canEnterPickedHotel);
        while (true) {
            debug("Looking for hotel.");
            this->randomSleep();
            this->sendHotelRequests(Alien::getRandomHotelId());
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

            updateClocks(msg);
            switch ((MessageType) status.MPI_TAG) {
                case HOTEL_REQUEST:
                    debug("Got HOTEL_REQUEST: clk %d, alien %d, hotel %d", msg.clock, msg.alienId, msg.hotelId);
                    sendHotelAck(msg.alienId);
                    removeOldAndAddNewRequest(msg);
                    break;
                case HOTEL_RELEASE:
                    debug("Got HOTEL_RELEASE: clk %d, alien %d", msg.clock, msg.alienId);
                    markThatAlienLeftHotel(msg);
                    break;
                case HOTEL_REQUEST_ACK:
                    debug("Got HOTEL_REQUEST_ACK: clk %d, alien %d", msg.clock, msg.alienId);
                    assert(processStatus == WAITING_TO_ENTER_HOTEL);
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
//    printf("THREAD SUPPORT: chcemy %d. Co otrzymamy?\n", provided);
    switch (provided) {
        case MPI_THREAD_SINGLE:
//            printf("Brak wsparcia dla w??tk??w, ko??cz??\n");
            /* Nie ma co, trzeba wychodzi?? */
            fprintf(stderr, "Brak wystarczaj??cego wsparcia dla w??tk??w - wychodz??!\n");
            MPI_Finalize();
            exit(-1);
            break;
        case MPI_THREAD_FUNNELED:
            printf("tylko te w??tki, ktore wykonaly mpi_init_thread mog?? wykona?? wo??ania do biblioteki mpi\n");
            break;
        case MPI_THREAD_SERIALIZED:
            /* Potrzebne zamki wok???? wywo??a?? biblioteki MPI */
            printf("tylko jeden watek naraz mo??e wykona?? wo??ania do biblioteki MPI\n");
            break;
        case MPI_THREAD_MULTIPLE:
            printf("Pe??ne wsparcie dla w??tk??w\n"); /* tego chcemy. Wszystkie inne powoduj?? problemy */
            break;
        default:
            printf("Nikt nic nie wie\n");
    }
}

int main(int argc, char **argv) {
    int rank;
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == 0) {
        // Print configuration
        check_thread_support(provided);
        int world_size;
        MPI_Comm_size(MPI_COMM_WORLD, &world_size);
        printf("Purple aliens: %d\n", PURPLES_COUNT);
        printf("Blue aliens: %d\n", BLUES_COUNT);
        printf("Hotels: %d\n", HOTELS_NUMBER);

        printf("Number of processes running: %d\n\n", world_size);

        if (world_size != ALL_ALIENS_COUNT) {
            printf("Wrong processes amount %d.\n", world_size);
            MPI_Abort(MPI_COMM_WORLD, 1);
        }

        if (PURPLES_COUNT <= 0 || BLUES_COUNT <= 0 || HOTELS_NUMBER <= 0) {
            printf("There has to be at least one of every process type and hotels.\n");
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
    }

    Alien *alien;
    if (rank >= FIRST_ID && rank <= LAST_ID) {
        alien = new Alien(rank);
    } else {
        printf("Wrong alienId: %d\n", rank);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    pthread_t thd;
    pthread_create(&thd, nullptr, &Entity::runComm, alien);
    alien->main();

    MPI_Finalize();
}