
#include <lsqecc/pipelines/slicer.hpp>

#include <exception>
#include <iostream>


int main(int argc, const char* argv[])
{
    try
    {
        return lsqecc::run_slicer_program(argc, argv, std::cin, std::cout, std::cerr);
    }
    catch(const std::exception& e)
    {
        std::cerr << "Compiler exception: " << e.what() << std::endl;
        return 1;
    }
}
