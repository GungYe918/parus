#include <iostream>
#include <slyte/Version.hpp>


int main(int argc, char** argv) {
    (void)argc; (void)argv;
    std::cout << slyte::k_version_string << "\n";
    return 0;
}