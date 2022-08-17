/**
 * @file stack-on-heap.cpp
 * @author Donghui (TK)
 * @brief   a Stack implemented with link-list of Node
 * @version 0.1
 * @date 2020-10-21
 */

#include <math.h>

#include <iostream>
#include <string>
#include <vector>

using namespace std;

class Node {
   private:
    int dataVal;
    // lastest item point to nothing
    Node* nextNodePtr;

   public:
    Node(int value, Node* nectNodePtr);
    Node(int value);
    int GetData();
    Node* GetNextNodePtr();
    void SetNextNodePtr(Node* nextNodePtr);
};

class Stack {
   private:
    Node* topNode;
    int nodeAmount;

   public:
    Stack();
    Stack(const Stack& original);
    ~Stack();
    bool isEmpty() const;
    int top() const;
    int pop();
    void push(int dataVal);
};

bool promptStack(Stack& onStack);
void listStack(Stack onStack);

// done
int main() {
    Stack onStack;
    while (promptStack(onStack)) { /* everything is handled in promptStack */
    };

    return 0;
}

// done
bool promptStack(Stack& onStack) {
    try {
        cout << "stack> ";
        string cmd;
        int value;

        cin >> cmd;
        if (cin.eof()) {
            return false;
        } else if (cmd.compare("end") == 0) {
            return false;
        } else if (cmd.compare("top") == 0) {
            cout << onStack.top() << "\n";
            return true;
        } else if (cmd.compare("pop") == 0) {
            cout << onStack.pop() << "\n";
            return true;
        } else if (cmd.compare("push") == 0) {
            cin >> value;
            if (cin.fail()) {
                cin.clear();
                cin.ignore(999, '\n');
                throw runtime_error("error: not a number");
            }
            onStack.push(value);
            cin.ignore(999, '\n');
        } else if (cmd.compare("list") == 0) {
            Stack copyStack = onStack;
            listStack(copyStack);
            return true;
        } else {
            cin.clear();
            cin.ignore(999, '\n');
            throw runtime_error("error: invalid command");
        }

    } catch (runtime_error& e) {
        cout << e.what() << '\n';
        return true;
    }
}

void listStack(Stack onStack) {
    if (onStack.isEmpty()) {
        cout << "[]\n";
        return;
    }

    cout << "[" << onStack.pop();
    while (!onStack.isEmpty()) {
        cout << ", " << onStack.pop();
    }
    cout << "]\n";
};

// Node methods
Node::Node(int value, Node* nextNodePtr) {
    this->dataVal = value;
    this->nextNodePtr = nextNodePtr;
}

Node::Node(int value) { this->dataVal = value; }

int Node::GetData() { return dataVal; }

Node* Node::GetNextNodePtr() { return nextNodePtr; }

void Node::SetNextNodePtr(Node* nextNodePtr) {
    this->nextNodePtr = nextNodePtr;
}

// Stack methods
// done
Stack::Stack() {
    topNode = nullptr;
    nodeAmount = 0;
}

Stack::~Stack() {
    // if stack being deconstructed, pop delete every node it has
    while (nodeAmount != 0) {
        this->pop();
    }
}

Stack::Stack(const Stack& original) {
    nodeAmount = original.nodeAmount;

    if (!original.isEmpty()) {
        // copy the top node data
        Node* originalPtr = original.topNode;
        topNode = new Node(originalPtr->GetData(), nullptr);

        originalPtr = originalPtr->GetNextNodePtr();
        Node* copyPtr = topNode;

        while (originalPtr != nullptr) {
            copyPtr->SetNextNodePtr(new Node(originalPtr->GetData(), nullptr));
            copyPtr = copyPtr->GetNextNodePtr();
            originalPtr = originalPtr->GetNextNodePtr();
        }
    } else {
        topNode = nullptr;
    }
}

bool Stack::isEmpty() const { return nodeAmount == 0 ? true : false; }

int Stack::top() const {
    if (isEmpty()) {
        throw runtime_error("error: stack is empty");
    }

    return topNode->GetData();
}

int Stack::pop() {
    if (isEmpty()) {
        throw runtime_error("error: stack is empty");
    }
    int tempData = topNode->GetData();
    // get the new topNode ptr
    Node* tempPtr = topNode->GetNextNodePtr();
    // delete the current node
    delete topNode;
    topNode = tempPtr;
    // update the new topNode
    nodeAmount--;

    return tempData;
}

void Stack::push(int value) {
    // new a node
    Node* newNodePrt = new Node(value, topNode);
    // replace the topNode's ptr
    topNode = newNodePrt;
    nodeAmount++;
}