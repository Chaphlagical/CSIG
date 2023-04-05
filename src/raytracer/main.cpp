#include <iostream>
#include "core/log.hpp"
#include "application.hpp"

int main(int argc, char **argv)
{
	ApplicationConfig config;
	Application application(config);

	application.run();

	return 0;
}