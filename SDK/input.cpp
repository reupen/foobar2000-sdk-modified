#include "foobar2000.h"
#include <exception>

service_ptr input_entry::open(const GUID & whatFor, file::ptr hint, const char * path, event_logger::ptr logger, abort_callback & aborter) {

#ifdef FOOBAR2000_DESKTOP
	if ( whatFor == input_stream_info_reader::class_guid ) {
		input_entry_v2::ptr v2;
		if ( v2 &= this ) {
			GUID g = v2->get_guid();
			service_enum_t<input_stream_info_reader_entry> e;
			service_ptr_t<input_stream_info_reader_entry> p;
			while (e.next(p)) {
				if (p->get_guid() == g) {
					return p->open( path, hint, aborter );
				}
			}
		}
		throw exception_io_unsupported_format();
	}
#endif
	if ( whatFor == album_art_extractor_instance::class_guid ) {
		input_entry_v2::ptr v2;
		if (v2 &= this) {
			GUID g = v2->get_guid();
			service_enum_t<album_art_extractor> e;
			album_art_extractor_v2::ptr p;
			while( e.next(p) ) {
				if ( p->get_guid() == g ) {
					return p->open( hint, path, aborter );
				}
			}
		}
		throw exception_io_unsupported_format();
	}
	if ( whatFor == album_art_editor_instance::class_guid ) {
		input_entry_v2::ptr v2;
		if (v2 &= this) {
			GUID g = v2->get_guid();
			service_enum_t<album_art_editor> e;
			album_art_editor_v2::ptr p;
			while( e.next(p) ) {
				if ( p->get_guid() == g ) {
					return p->open( hint, path, aborter );
				}
			}
		}
		throw exception_io_unsupported_format();
	}

	input_entry_v3::ptr v3;

	if (v3 &= this) {
		return v3->open_v3( whatFor, hint, path, logger, aborter );
	} else {
		if (whatFor == input_decoder::class_guid) {
			input_decoder::ptr obj;
			open(obj, hint, path, aborter);
			if ( logger.is_valid() ) {
				input_decoder_v2::ptr v2;
				if (v2 &= obj) v2->set_logger(logger);
			}
			return obj;
		}
		if (whatFor == input_info_reader::class_guid) {
			input_info_reader::ptr obj;
			open(obj, hint, path, aborter);
			return obj;
		}
		if (whatFor == input_info_writer::class_guid) {
			input_info_writer::ptr obj;
			open(obj, hint, path, aborter);
			return obj;
		}
	}

	throw pfc::exception_not_implemented();
}

bool input_entry::g_find_service_by_path(service_ptr_t<input_entry> & p_out,const char * p_path)
{
    auto ext = pfc::string_extension(p_path);
    return g_find_service_by_path(p_out, p_path, ext );
}

bool input_entry::g_find_service_by_path(service_ptr_t<input_entry> & p_out,const char * p_path, const char * p_ext)
{
	service_ptr_t<input_entry> ptr;
	service_enum_t<input_entry> e;
	while(e.next(ptr))
	{
		if (ptr->is_our_path(p_path,p_ext))
		{
			p_out = ptr;
			return true;
		}
	}
	return false;
}

bool input_entry::g_find_service_by_content_type(service_ptr_t<input_entry> & p_out,const char * p_content_type)
{
	service_ptr_t<input_entry> ptr;
	service_enum_t<input_entry> e;
	while(e.next(ptr))
	{
		if (ptr->is_our_content_type(p_content_type))
		{
			p_out = ptr;
			return true;
		}
	}
	return false;
}


#if 0
static void prepare_for_open(service_ptr_t<input_entry> & p_service,service_ptr_t<file> & p_file,const char * p_path,filesystem::t_open_mode p_open_mode,abort_callback & p_abort,bool p_from_redirect)
{
	if (p_file.is_empty())
	{
		service_ptr_t<filesystem> fs;
		if (filesystem::g_get_interface(fs,p_path)) {
			if (fs->supports_content_types()) {
				fs->open(p_file,p_path,p_open_mode,p_abort);
			}
		}
	}

	if (p_file.is_valid())
	{
		pfc::string8 content_type;
		if (p_file->get_content_type(content_type))
		{
			if (input_entry::g_find_service_by_content_type(p_service,content_type))
				return;
		}
	}

	if (input_entry::g_find_service_by_path(p_service,p_path))
	{
		if (p_from_redirect && p_service->is_redirect()) throw exception_io_unsupported_format();
		return;
	}

	throw exception_io_unsupported_format();
}
#endif

bool input_entry::g_find_inputs_by_content_type(pfc::list_base_t<service_ptr_t<input_entry> > & p_out, const char * p_content_type, bool p_from_redirect) {
	service_enum_t<input_entry> e;
	service_ptr_t<input_entry> ptr;
	bool ret = false;
	while (e.next(ptr)) {
		if (!(p_from_redirect && ptr->is_redirect())) {
			if (ptr->is_our_content_type(p_content_type)) { p_out.add_item(ptr); ret = true; }
		}
	}
	return ret;
}

bool input_entry::g_find_inputs_by_path(pfc::list_base_t<service_ptr_t<input_entry> > & p_out, const char * p_path, bool p_from_redirect) {
	service_enum_t<input_entry> e;
	service_ptr_t<input_entry> ptr;
	auto extension = pfc::string_extension(p_path);
	bool ret = false;
	while (e.next(ptr)) {
		if (!(p_from_redirect && ptr->is_redirect())) {
			if (ptr->is_our_path(p_path, extension)) { p_out.add_item(ptr); ret = true; }
		}
	}
	return ret;
}

static GUID input_get_guid( input_entry::ptr e ) {
#ifdef FOOBAR2000_DESKTOP
	input_entry_v2::ptr p;
	if ( p &= e ) return p->get_guid();
#endif
	return pfc::guid_null;
}

service_ptr input_entry::g_open_from_list(input_entry_list_t const & p_list, const GUID & whatFor, service_ptr_t<file> p_filehint, const char * p_path, event_logger::ptr logger, abort_callback & p_abort, GUID * outGUID) {
	const t_size count = p_list.get_count();
	if ( count == 0 ) {
		// sanity
		throw exception_io_unsupported_format();
	} else if (count == 1) {
		auto ret = p_list[0]->open(whatFor, p_filehint, p_path, logger, p_abort);
		if ( outGUID != nullptr ) * outGUID = input_get_guid( p_list[0] );
		return ret;
	} else {
		std::exception_ptr errData, errUnsupported;
		for (t_size n = 0; n < count; n++) {
			try {
				auto ret = p_list[n]->open(whatFor, p_filehint, p_path, logger, p_abort);
				if (outGUID != nullptr) * outGUID = input_get_guid(p_list[n]);
				return ret;
			} catch (exception_io_no_handler_for_path) {
				//do nothing, skip over
			} catch(exception_io_unsupported_format) {
				if (!errUnsupported) errUnsupported = std::current_exception();
			} catch (exception_io_data) {
				if (!errData) errData = std::current_exception();
			}
		}
		if (errData) std::rethrow_exception(errData);
		if (errUnsupported) std::rethrow_exception(errUnsupported);
		throw exception_io_unsupported_format();
	}
}

#ifdef FOOBAR2000_DESKTOP
service_ptr input_manager::open_v2(const GUID & whatFor, file::ptr hint, const char * path, bool fromRedirect, event_logger::ptr logger, abort_callback & aborter, GUID * outUsedEntry) {
	// We're wrapping open_v2() on top of old open().
	// Assert on GUIDs that old open() is known to recognize.
	PFC_ASSERT(whatFor == input_decoder::class_guid || whatFor == input_info_reader::class_guid || whatFor == input_info_writer::class_guid || whatFor == input_stream_selector::class_guid);

    {
        input_manager_v2::ptr v2;
        if ( v2 &= this ) {
            return v2->open_v2( whatFor, hint, path, fromRedirect, logger, aborter, outUsedEntry );
        }
    }

	auto ret = open( whatFor, hint, path, fromRedirect, aborter, outUsedEntry );

#ifdef FB2K_HAVE_EVENT_LOGGER
	if ( logger.is_valid() ) {
		input_decoder_v2::ptr dec;
		if (dec &= ret) {
			dec->set_logger(logger);
		}
	}
#endif
	return ret;
}
#endif

service_ptr input_entry::g_open(const GUID & whatFor, file::ptr p_filehint, const char * p_path, event_logger::ptr logger, abort_callback & p_abort, bool p_from_redirect) {

#ifdef FOOBAR2000_DESKTOP

	// #define rationale: not all FOOBAR2000_MODERN flavours come with input_manager implementation, but classic 1.4+ does
#if !defined(FOOBAR2000_MODERN) && FOOBAR2000_TARGET_VERSION >= 79

#if FOOBAR2000_TARGET_VERSION > 79
	return input_manager_v2::get()->open_v2(whatFor, p_filehint, p_path, p_from_redirect, logger, p_abort);
#else
	return input_manager::get()->open_v2(whatFor, p_filehint, p_path, p_from_redirect, logger, p_abort);
#endif

#endif
	{
		input_manager::ptr m;
		service_enum_t<input_manager> e;
		if (e.next(m)) {
			return m->open_v2(whatFor, p_filehint, p_path, p_from_redirect, logger, p_abort);
		}
	}
#endif

	const bool needWriteAcecss = !!(whatFor == input_info_writer::class_guid);

	service_ptr_t<file> l_file = p_filehint;
	if (l_file.is_empty()) {
		service_ptr_t<filesystem> fs;
		if (filesystem::g_get_interface(fs, p_path)) {
			if (fs->supports_content_types()) {
				fs->open(l_file, p_path, needWriteAcecss ? filesystem::open_mode_write_existing : filesystem::open_mode_read, p_abort);
			}
		}
	}

	if (l_file.is_valid()) {
		pfc::string8 content_type;
		if (l_file->get_content_type(content_type)) {
			pfc::list_t< input_entry::ptr > list;
#if PFC_DEBUG
			FB2K_DebugLog() << "attempting input open by content type: " << content_type;
#endif
			if (g_find_inputs_by_content_type(list, content_type, p_from_redirect)) {
				try {
					return g_open_from_list(list, whatFor, l_file, p_path, logger, p_abort);
				} catch (exception_io_unsupported_format) {
#if PFC_DEBUG
					FB2K_DebugLog() << "Failed to open by content type, using fallback";
#endif
				}
			}
		}
	}

#if PFC_DEBUG
	FB2K_DebugLog() << "attempting input open by path: " << p_path;
#endif
	{
		pfc::list_t< input_entry::ptr > list;
		if (g_find_inputs_by_path(list, p_path, p_from_redirect)) {
			return g_open_from_list(list, whatFor, l_file, p_path, logger, p_abort);
		}
	}

	throw exception_io_unsupported_format();
}

void input_entry::g_open_for_decoding(service_ptr_t<input_decoder> & p_instance,service_ptr_t<file> p_filehint,const char * p_path,abort_callback & p_abort,bool p_from_redirect) {
	TRACK_CALL_TEXT("input_entry::g_open_for_decoding");
	p_instance ^= g_open(input_decoder::class_guid, p_filehint, p_path, nullptr, p_abort, p_from_redirect);
}

void input_entry::g_open_for_info_read(service_ptr_t<input_info_reader> & p_instance,service_ptr_t<file> p_filehint,const char * p_path,abort_callback & p_abort,bool p_from_redirect) {
	TRACK_CALL_TEXT("input_entry::g_open_for_info_read");
	p_instance ^= g_open(input_info_reader::class_guid, p_filehint, p_path, nullptr, p_abort, p_from_redirect);
}

void input_entry::g_open_for_info_write(service_ptr_t<input_info_writer> & p_instance,service_ptr_t<file> p_filehint,const char * p_path,abort_callback & p_abort,bool p_from_redirect) {
	TRACK_CALL_TEXT("input_entry::g_open_for_info_write");
	p_instance ^= g_open(input_info_writer::class_guid, p_filehint, p_path, nullptr, p_abort, p_from_redirect);
}

void input_entry::g_open_for_info_write_timeout(service_ptr_t<input_info_writer> & p_instance,service_ptr_t<file> p_filehint,const char * p_path,abort_callback & p_abort,double p_timeout,bool p_from_redirect) {
	pfc::lores_timer timer;
	timer.start();
	for(;;) {
		try {
			g_open_for_info_write(p_instance,p_filehint,p_path,p_abort,p_from_redirect);
			break;
		} catch(exception_io_sharing_violation) {
			if (timer.query() > p_timeout) throw;
			p_abort.sleep(0.01);
		}
	}
}

bool input_entry::g_is_supported_path(const char * p_path)
{
	service_ptr_t<input_entry> ptr;
	service_enum_t<input_entry> e;
	auto ext = pfc::string_extension (p_path);
	while(e.next(ptr))
	{
		if (ptr->is_our_path(p_path,ext)) return true;
	}
	return false;
}



void input_open_file_helper(service_ptr_t<file> & p_file,const char * p_path,t_input_open_reason p_reason,abort_callback & p_abort)
{
	if (p_file.is_empty()) {
		switch(p_reason) {
		default:
			uBugCheck();
		case input_open_info_read:
		case input_open_decode:
			filesystem::g_open(p_file,p_path,filesystem::open_mode_read,p_abort);
			break;
		case input_open_info_write:
			filesystem::g_open(p_file,p_path,filesystem::open_mode_write_existing,p_abort);
			break;
		}
	} else {
		p_file->reopen(p_abort);
	}
}

bool input_entry::g_are_parallel_reads_slow(const char * path) {
	auto ext = pfc::string_extension(path);
	input_entry::ptr svc;
	service_enum_t<input_entry> e;
	while (e.next(svc)) {
		if (svc->is_our_path(path, ext) && svc->are_parallel_reads_slow()) return true;
	}
	return false;
}

void input_entry_v3::open_for_decoding(service_ptr_t<input_decoder> & p_instance, service_ptr_t<file> p_filehint, const char * p_path, abort_callback & p_abort) {
	p_instance ^= open_v3( input_decoder::class_guid, p_filehint, p_path, nullptr, p_abort );
}
void input_entry_v3::open_for_info_read(service_ptr_t<input_info_reader> & p_instance, service_ptr_t<file> p_filehint, const char * p_path, abort_callback & p_abort) {
	p_instance ^= open_v3(input_info_reader::class_guid, p_filehint, p_path, nullptr, p_abort);
}
void input_entry_v3::open_for_info_write(service_ptr_t<input_info_writer> & p_instance, service_ptr_t<file> p_filehint, const char * p_path, abort_callback & p_abort) {
	p_instance ^= open_v3(input_info_writer::class_guid, p_filehint, p_path, nullptr, p_abort);
}
