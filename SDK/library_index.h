#pragma once

#include "search_tools.h"

//! \since 2.0
class NOVTABLE library_index : public service_base {
	FB2K_MAKE_SERVICE_COREAPI(library_index);
public:
	virtual void hit_test(search_filter::ptr pattern, metadb_handle_list_cref items, bool* out, abort_callback & a) = 0;

	enum {
		flag_sort = 1 << 0,
	};

	//! @returns list of metadb_handles. Safe to use arr->as_list_of<metadb_handle>() to get a pfc::list_base_const_t<metadb_handle_ptr>
	virtual fb2k::arrayRef search(search_filter::ptr pattern, uint32_t flags, abort_callback& abort) = 0;
};