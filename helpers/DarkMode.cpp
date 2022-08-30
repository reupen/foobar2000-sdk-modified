#include "StdAfx.h"
#include "DarkMode.h"

bool fb2k::isDarkMode() {
	return ui_config_manager::get()->is_dark_mode();
}
