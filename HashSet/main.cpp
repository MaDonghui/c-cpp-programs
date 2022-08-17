#include <cassert>
#include <iostream>
#include <string>

#include "HashSet.h"

/**
 * The main function
 * compile and tested under c14
 */
int main() {
    // create your set
    HashSet<std::string> set;
    // add some data
    set.add(std::string("Some data"));
    // check some data
    std::cout << (set.contains(std::string("Some data")) ? "yes" : "no")
              << std::endl;
    std::cout << (set.contains(std::string("some data")) ? "yes" : "no")
              << std::endl;

    // remove some data
    std::cout << (set.remove(std::string("Some data")) ? "contained"
                                                       : "did not contain")
              << std::endl;
    std::cout << (set.remove(std::string("some data")) ? "contained"
                                                       : "did not contain")
              << std::endl;

    std::cout << "Checkpoint 1: original tester passed" << std::endl;
    // done

    // additional tests

    // Basic Set features tests, no 100% coverage i think :p
    HashSet<std::string> basicStringSet;
    assert(("Insert String: \"1\"." && basicStringSet.add("1") == true));
    assert(("Exists String\"1\"." && basicStringSet.contains("1") == true));
    assert(("No Exists String\"2\"." && basicStringSet.contains("2") == false));

    assert(("Remove \"1\"." && basicStringSet.remove("1") == true));
    assert(("Remove None Exist \"2\"." && basicStringSet.remove("2") == false));

    basicStringSet.add("Duplicated");
    assert(("Insert Duplicated String." &&
            basicStringSet.add("Duplicated") == false));

    HashSet<int> original;
    original.add(1);
    original.add(2);
    original.add(3);
    original.add(4);

    std::cout << "Checkpoint 2: insert, contain, remove passed" << std::endl;

    HashSet<int> copy1 = HashSet<int>(original);
    assert(("copy1 contain 1" && copy1.contains(1) == true));
    assert(("copy1 contain 2" && copy1.contains(2) == true));
    assert(("copy1 contain 3" && copy1.contains(3) == true));
    assert(("copy1 contain 4" && copy1.contains(4) == true));

    HashSet<int> copy2 = original;
    assert(("copy2 contain 1" && copy2.contains(1) == true));
    assert(("copy2 contain 2" && copy2.contains(2) == true));
    assert(("copy2 contain 3" && copy2.contains(3) == true));
    assert(("copy2 contain 4" && copy2.contains(4) == true));

    std::cout << "Checkpoint 3: copy & move constructor Passed" << std::endl;

    // iterator tests
    std::cout << "Checkpoint 4: Iterator, No assertion here" << std::endl;
    HashSet<std::string> iteTestSet;
    for (size_t i = 0; i < 7; i++) {
        iteTestSet.add(std::to_string(i));
    }
    for (auto it = iteTestSet.begin(); it != iteTestSet.end(); ++it) {
        std::cout << *it << " ";
    }
    std::cout << std::endl;

    std::cout << "add even more, and expect an rehash (order doesn't change on int ofc)"
              << std::endl;

    for (size_t i = 7; i < 20; i++) {
        iteTestSet.add(std::to_string(i));
    }
    for (auto it = iteTestSet.begin(); it != iteTestSet.end(); ++it) {
        std::cout << *it << " ";
    }
    std::cout << std::endl;
    // no assert above, this test doesn't spark joy :v

    auto itLhs = iteTestSet.begin();
    auto itRhs = iteTestSet.begin();
    assert(("Iterator ==" && itLhs == itRhs));

    auto itEnd = iteTestSet.end();
    assert(("Iterator !=" && itLhs != itEnd));

    return 0;
}
