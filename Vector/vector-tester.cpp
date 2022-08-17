#include <iostream>
#include "diyvector.h"

class TestFailed{
    public:
        TestFailed(int seq){
            sequenceNumber = seq;
        }
        int sequenceNumber;
};

int testNumber = 0;

#define check(CALL) { testNumber++; if ( !(CALL) ) throw TestFailed(testNumber); }

#define checkException(CALL){ \
    testNumber++; \
    bool exceptionRaised = false; \
    try{ \
        CALL; \
    }catch(DiyVector<int>::OutOfRange& o){ \
        exceptionRaised = true; \
    } \
    if ( !exceptionRaised ) throw TestFailed(testNumber); \
}

int main(){
    try{
        DiyVector<int> v;

        check(v.size() == 0);  // test 1
        checkException(v.at(0));

        v.pushBack(42);
        check(v.size() == 1);
        check(v.at(0) == 42);
        checkException(v.at(1)); // test 5
 
        v.pushBack(43);
        check(v.size() == 2);
        check(v.at(1) == 43);
        check(v.at(0) == 42);

        v.popBack();
        v.popBack();
        check(v.size()==0);

        checkException(v.popBack()); // test 10
        v.pushBack(142);
        v.pushBack(143);
        v.pushBack(144);
        check(v.size()==3);
        check(v.at(0)==142);
        check(v.at(1)==143);
        check(v.at(2)==144);
        checkException(v.at(3)); // test 15

        v.at(0) = 17;
        check(v.at(0)==17);

        checkException(v.erase(3));
        checkException(v.erase(42));
        v.erase(1);
        check(v.size()==2);
        check(v.at(0)==17);  // test 20
        check(v.at(1)==144);

        v.pushBack('A');
        v.pushBack('B');
        check(v.size()==4);
        check(v.at(2)==65);
        check(v.at(3)==66);

        v.insert(2, 22);
        check(v.at(0)==17); // test 25
        check(v.at(1)==144);
        check(v.at(2)==22);
        check(v.at(3)==65);
        check(v.at(4)==66);
        check(v.size()==5); // test 30

        DiyVector<int> v2;
        v2.insert(0,42);
        v2.pushBack(11);
        v2.insert(0,44);
        check(v2.size()==3);
        check(v2.at(0)==44);
        check(v2.at(1)==42);
        check(v2.at(2)==11);
        v2.popBack();
        v2.insert(0,99);
        check(v2.size()==3); // test 35
        check(v2.at(0)==99);
        check(v2.at(1)==44);
        check(v2.at(2)==42);

        DiyVector<int> v3;
        v3.pushBack(1);
        v3.erase(0);
        checkException(v3.at(0)); 
        check(v3.size()==0); // test 40
        checkException(v3.insert(1,-5));
        check(v3.size()==0); // test 42
 
        std::cout << "All tests passed!\n";
    }
    catch(TestFailed& tf){
        std::cerr << "Test number " << tf.sequenceNumber << " failed.\n";
        return 1;
    }
    catch(...){
        std::cerr << "an unexpected exception occured\n";
        std::cerr << "Tests passed so far: " << testNumber << std::endl;
        return 2;
    }
    return 0;
}