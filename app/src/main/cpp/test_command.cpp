#include <iostream>

int main(int argc, char** argv) {
    std::cout << "alr-test-command ok";
    for (int i = 1; i < argc; ++i) {
        std::cout << " arg" << i << "=" << argv[i];
    }
    std::cout << "\n";
    return 0;
}
