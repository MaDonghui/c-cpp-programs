/**
 * @file diyvector.h
 * @author Donghui (TK)
 * @brief a vector implemented with array
 * @date 2019-10-21

 */

#include <iostream>
using namespace std;

template <typename T>
class DiyVector {
   public:
    DiyVector();
    ~DiyVector();

    T& at(unsigned int index) const;
    // item at index
    // throws OutOfRange

    unsigned int size() const;
    // number of items in the vector

    void pushBack(const T& item);
    // append item at the end of vector

    void popBack();
    // remove item at the end of vector
    // throws OutOfRange

    void erase(unsigned int index);
    // remove element at index
    // throws OutOfRange

    void insert(unsigned int index, const T& item);
    // insert item before element at index, with:
    // 0 <= index <= size()
    // throws OutOfRange

    class OutOfRange {};

   private:
    // accessible is vector size
    // actual is array size
    int vectorSize, actualSize;
    T* arrayPtr;
};

template <typename T>
DiyVector<T>::DiyVector() {
    vectorSize = 0;
    actualSize = 0;
    arrayPtr = new T[0];
}

template <typename T>
DiyVector<T>::~DiyVector() {
    delete[] arrayPtr;
}

template <typename T>
T& DiyVector<T>::at(unsigned int index) const {
    if (!((0 <= index) && (index < vectorSize))) {
        throw OutOfRange();
    }

    return arrayPtr[index];
}

template <typename T>
unsigned int DiyVector<T>::size() const {
    return vectorSize;
}

template <typename T>
void DiyVector<T>::pushBack(const T& item) {
    if (actualSize == vectorSize) {
        // no more space left
        // create a new actualSize + 1 size array and update the actual slots
        T* newArrayPtr = new T[++actualSize];

        // copy then delete old one
        for (int i = 0; i < vectorSize; ++i) {
            newArrayPtr[i] = arrayPtr[i];
        }
        delete[] arrayPtr;

        // update array pointer
        arrayPtr = newArrayPtr;
    }
    // update vector size
    ++vectorSize;
    // assign item to last slot position
    arrayPtr[vectorSize - 1] = item;
}

template <typename T>
void DiyVector<T>::popBack() {
    if (vectorSize == 0) {
        throw OutOfRange();
    }

    --vectorSize;
}

template <typename T>
void DiyVector<T>::erase(unsigned int index) {
    // if vector size is 0 or index is not within vector range, throw error
    if (vectorSize == 0) {
        throw OutOfRange();
    }
    if (!((0 <= index) && (index < vectorSize))) {
        throw OutOfRange();
    }

    // move every elements after index one slot ahead
    for (int i = index; i < vectorSize; ++i) {
        arrayPtr[index] = arrayPtr[index + 1];
    }

    // simply change the accessible range of vector, consider it erased
    --vectorSize;
}

template <typename T>
void DiyVector<T>::insert(unsigned int index, const T& item) {
    if (!((0 <= index) && (index <= vectorSize))) {
        throw OutOfRange();
    }

    // if no more avaliable spot left, increase size by 1
    if (actualSize == vectorSize) {
        // no more space left
        // create a new actualSize + 1 size array and update the actual slots
        T* newArrayPtr = new T[++actualSize];

        // copy then delete old one
        for (int i = 0; i < vectorSize; ++i) {
            newArrayPtr[i] = arrayPtr[i];
        }
        delete[] arrayPtr;

        // update array pointer
        arrayPtr = newArrayPtr;
    }

    // change the vector size for moving first
    ++vectorSize;
    // move all elements from target index including index to right by 1
    for (int i = vectorSize - 1; i > index; --i) {
        arrayPtr[i] = arrayPtr[i - 1];
    }

    // insert the items (really is just overwrite the index position)
    arrayPtr[index] = item;
}
