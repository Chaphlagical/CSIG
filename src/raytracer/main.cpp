#include "application.hpp"
#include "core/log.hpp"
#include <iostream>

#include <glm/gtc/matrix_transform.hpp>

int main(int argc, char **argv)
{
	ApplicationConfig config;

	while (true)
	{
		Application application(config);

		auto restartParams = application.run();
		if (restartParams.has_value())
		{
			config = restartParams.value();
		}
		else
		{
			break;
		}
	}


	return 0;
}