#pragma once

#include <3ds.h>
#include <stdio.h>

extern PrintConsole console;

inline void printheader()
{
    puts("\e[0;0H\e[0m\e[4msocks\e[0m \e[1mv0.0_dev0\e[0m \e[36mby Sonomi-nyan~\e[0m");
}
