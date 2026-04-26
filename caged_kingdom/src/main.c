/*
 * main.c  JCE application entry point.
 *
 * All subsystem creation, asset loading, and game logic live
 * in the engine and game/ck_app respectively.
 *
 * This file uses the JCE_MAIN macro — no direct SDL dependency.
 */

#include <jce/application/jce_main.h>
#include "game/ck_app.h"

JCE_MAIN(ck_app_get_desc)
