#include <gui/model/Model.hpp>
#include <gui/model/ModelListener.hpp>
#include <touchgfx/Utils.hpp>

#ifndef SIMULATOR
extern "C" {
#include "main.h"
}
#endif

Model::Model() : modelListener(0)
{

}

void Model::tick()
{

}

void Model::toggleGpio()
{
#ifndef SIMULATOR
    HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin);
#else
    touchgfx_printf("GPIO toggle requested\n");
#endif
}
