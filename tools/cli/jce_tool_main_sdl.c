/* Sanctioned CLI entry bridge. SDL supplies UTF-8 argv where required.
 * No renderer, event loop, engine initialization or application main archive. */
#include "jce_tool_main.h"
#include <SDL3/SDL_main.h>

int main(int argc, char **argv)
{
    return jce_tool_main(argc, argv);
}
