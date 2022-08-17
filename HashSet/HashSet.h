#if !defined(HASHSET_H)
#define HASHSET_H

#include <functional>
#include <map>
#include <vector>

using std::find;
using std::hash;
using std::map;
using std::vector;

/**
 * @brief A Set implemented with hash-table, with a custom (matrix) iterator
 * HashTable size is dynamic and hash collision tolerant
 * iterable, near constant access time, templated type.
 * Support copy constructor
 * Support move operator (default due to all STL std container)
 * @tparam T datatype in container
 */
template <typename T>
class HashSet {
   private:
    vector<vector<T>> hashTable;
    size_t elementCount;
    uint32_t calcHashIdx(const T& data);

   public:
    struct Iterator;

    bool add(const T& data);
    bool contains(const T& data);
    bool remove(const T& data);
    size_t size() { return this->elementCount; };

    HashSet();
    HashSet(const HashSet<T>& source);
    HashSet& operator=(HashSet<T>&& source) = default;

    // Iterator begin & end
    Iterator begin();
    Iterator end() { return Iterator(nullptr, 0, 0); }
};

// Iterator relates

/**
 * @brief an input Iterator for HashSet
 * capable of iterating through hash table
 * support copy construct, copy assign, destruct, dereference, pre/post increase
 * equality, inequality
 * @tparam T datatype
 */
template <typename T>
struct HashSet<T>::Iterator {
    using iterator_category = std::input_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = T;
    using pointer = T*;
    using reference = T&;

    Iterator(vector<vector<T>>* pHashTable, size_t idxInner, size_t idxOuter)
        : pHashTable(pHashTable), idxInner(idxInner), idxOuter(idxOuter) {}
    ~Iterator() = default;

    // member functions
    reference operator*() const {
        return pHashTable->at(idxOuter).at(idxInner);
    }  // dereference
    pointer operator->() { return pHashTable->at(idxOuter).at(idxInner); }

    // prefix increment
    Iterator& operator++() {
        // if reach the end of a bucket
        if (idxInner + 1 >= pHashTable->at(idxOuter).size()) {
            idxInner = 0;
            // find next none empty bucket
            for (++idxOuter; idxOuter < pHashTable->size(); ++idxOuter) {
                if (!pHashTable->at(idxOuter).empty()) return *this;
            }
        } else {
            idxInner++;
            return *this;
        }

        // no more elements left, return HashSet::end()
        pHashTable = nullptr;
        idxInner = 0;
        idxOuter = 0;
        return *this;
    };

    // postfix increment
    Iterator& operator++(int) {
        Iterator tmp = *this;
        ++(*this);
        return tmp;
    };

    // non-member functions
    friend bool operator==(const Iterator& lhs, const Iterator& rhs) {
        return (lhs.pHashTable == rhs.pHashTable) &&
               (lhs.idxInner == rhs.idxInner) && (lhs.idxOuter == rhs.idxOuter);
    }

    friend bool operator!=(const Iterator& lhs, const Iterator& rhs) {
        return (lhs.pHashTable != rhs.pHashTable) ||
               (lhs.idxInner != rhs.idxInner) || (lhs.idxOuter != rhs.idxOuter);
    }

   private:
    // object pointer
    vector<vector<T>>* pHashTable = nullptr;
    size_t idxInner = 0;
    size_t idxOuter = 0;
};

/**
 * @brief create an iterator
 * points to the first element in HashSet's hash table or and end iterator
 *
 * @tparam T
 * @return HashSet<T>::Iterator the iterator points to first element
 * (end if no elements exists)
 */
template <typename T>
typename HashSet<T>::Iterator HashSet<T>::begin() {
    // find the first available item
    for (size_t i = 0; i < this->hashTable.size(); i++) {
        if (!this->hashTable.at(i).empty()) {
            return Iterator(&this->hashTable, 0, i);
        }
    }

    return Iterator(nullptr, 0, 0);
}

// HashSet relates

/**
 * @brief Construct a new Hash Set< T>:: Hash Set object
 *
 * @tparam T datatype
 */
template <typename T>
HashSet<T>::HashSet() {
    this->hashTable = vector<vector<T>>(32, vector<T>());
    this->elementCount = 0;
}

/**
 * @brief cope constructor
 *
 * @tparam T
 * @param source source HashSet
 */
template <typename T>
HashSet<T>::HashSet(const HashSet<T>& source) {
    for (auto const bucket : source.hashTable) {
        this->hashTable.push_back(bucket);
    }
}

/**
 * @brief add a new unique element to HashSet
 *
 * @tparam T datatype
 * @param data new element
 * @return true successfully inserted new element
 * @return false failed to insert new element, it is not unique
 */
template <typename T>
bool HashSet<T>::add(const T& data) {
    uint32_t bucketIdx = calcHashIdx(data);
    vector<T>* pBucket = &this->hashTable.at(bucketIdx);

    // if element exists, fail to insert
    if (find(pBucket->begin(), pBucket->end(), data) != pBucket->end()) {
        return false;
    }

    /**
     * before inserting unique item, check if rehash needed
     * dynamically manage hashtable size, ideally one bucket one item for
     * constant access time
     **/
    if (this->elementCount + 1 >= this->hashTable.size()) {
        std::cout << "rehash\n" << std::endl;

        vector<vector<T>> newHashTable =
            vector<vector<T>>(this->hashTable.size() * 2, vector<T>());

        for (auto const& bucket : this->hashTable) {
            for (auto const& item : bucket) {
                newHashTable.at(calcHashIdx(item)).push_back(item);
            }
        }

        this->hashTable = newHashTable;
    }

    // recalculate hashIdx is necessary, as hash table may have resized
    this->hashTable.at(calcHashIdx(data)).push_back(data);
    this->elementCount++;

    return true;
}

/**
 * @brief determine if a certain element exists in HashSet
 *
 * @tparam T datatype
 * @param data target element
 * @return true element exists in HashSet
 * @return false element does not exist in HashSet
 */
template <typename T>
bool HashSet<T>::contains(const T& data) {
    uint32_t bucketIdx = calcHashIdx(data);
    vector<T>* pBucket = &this->hashTable.at(bucketIdx);

    if (find(pBucket->begin(), pBucket->end(), data) != pBucket->end()) {
        return true;
    }

    return false;
}

/**
 * @brief remove the match element from set (if exists)
 *
 * @tparam T datatype
 * @param data target data
 * @return true found element and removed
 * @return false element does not exist in set
 */
template <typename T>
bool HashSet<T>::remove(const T& data) {
    uint32_t bucketIdx = calcHashIdx(data);
    vector<T>* pBucket = &this->hashTable.at(bucketIdx);

    auto position = find(pBucket->begin(), pBucket->end(), data);
    if (position != pBucket->end()) {
        pBucket->erase(position);
        return true;
    } else {
        return false;
    }
}

/**
 * @brief using std::hash and modulo of buckets count
 * to calculate bucket index
 *
 * @tparam T datatype
 * @param data data to hash
 * @return uint32_t
 */
template <typename T>
uint32_t HashSet<T>::calcHashIdx(const T& data) {
    using std::hash;
    hash<T> dataHash;

    return dataHash(data) % this->hashTable.size();
}

#endif  // HASHSET_H
