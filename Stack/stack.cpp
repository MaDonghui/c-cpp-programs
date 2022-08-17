/**
 * @file stack.cpp
 * @author Donghui (TK)
 * @brief a stack implemented with vector
 * @date 2019-10-14
 * @copyright Copyright (c) 2022
 */

#include <math.h>

#include <iostream>
#include <string>
#include <vector>

using namespace std;

class Stack {
   public:
    bool isEmpty();
    // returns true if stack has no elements stored

    int top();
    // returns element from top of the stack
    // throws runtime_error("stack is empty")

    int pop();
    // returns element from top of the stack and removes it
    // throws runtime_error("stack is empty")

    void push(int value);
    // puts a new element on top of the stack

   private:
    vector<int> elements;
};

bool promptStack(Stack& onStack);
void listStack(Stack onStack);

int main() {
    Stack onStack;
    while (promptStack(onStack)) { /* everything is handled in promptStack */
    };

    return 0;
}

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
            listStack(onStack);
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

bool Stack::isEmpty() { return elements.size() == 0 ? true : false; }

int Stack::top() {
    if (isEmpty()) {
        throw runtime_error("error: stack is empty");
        /* code */
    }

    return elements.back();
}

int Stack::pop() {
    if (isEmpty()) {
        throw runtime_error("error: stack is empty");
    }

    int temp = elements.back();
    elements.pop_back();
    return temp;
}

void Stack::push(int value) { elements.push_back(value); }