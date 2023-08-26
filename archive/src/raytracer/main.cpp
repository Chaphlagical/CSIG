#include "application.hpp"
#include "core/log.hpp"
#include <iostream>

#include <glm/gtc/matrix_transform.hpp>

int main(int argc, char **argv)
{
	ApplicationConfig config;

	Application application(config);

	application.run();

	return 0;
}