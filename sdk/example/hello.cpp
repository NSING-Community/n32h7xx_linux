// SPDX-License-Identifier: BSD-3-Clause
#include <iostream>
#include <thread>
int main()
{
    int result = 0;
    std::thread worker([&result] { result = 42; });
    worker.join();
    if (result != 42) return 1;
    std::cout << "PASS standard SDK C++ application\n";
}
