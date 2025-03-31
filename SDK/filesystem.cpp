#include "foobar2000-sdk-pch.h"
#include "filesystem.h"
#include "unpack.h"
#include "archive.h"
#include "hasher_md5.h"
#include "mem_block_container.h"
#include "filesystem_transacted.h"

static constexpr char unpack_prefix[] = "unpack://";
static constexpr unsigned unpack_prefix_len = 9;


#ifndef _WIN32
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#endif

void unpacker::g_open(service_ptr_t<file> & p_out,const service_ptr_t<file> & p,abort_callback & p_abort)
{
	for (auto ptr : enumerate()) {
		p->reopen(p_abort);
		try {
			ptr->open(p_out, p, p_abort);
			return;
		} catch (exception_io_data const&) {}
	}
	throw exception_io_data();
}

void file::seek_probe(t_filesize p_position, abort_callback & p_abort) {
	try { seek(p_position, p_abort); } catch(exception_io_seek_out_of_range const &) {throw exception_io_data();}
}

void file::seek_ex(t_sfilesize p_position, file::t_seek_mode p_mode, abort_callback &p_abort) {
	switch(p_mode) {
	case seek_from_beginning:
		seek(p_position,p_abort);
		break;
	case seek_from_current:
		seek(p_position + get_position(p_abort),p_abort);
		break;
	case seek_from_eof:
		seek(p_position + get_size_ex(p_abort),p_abort);
		break;
	default:
		throw exception_io_data();
	}
}

static void makeBuffer(pfc::array_t<uint8_t> & buffer, size_t size) {
	for(;;) {// Tolerant malloc - allocate a smaller buffer if we're unable to acquire the requested size.
		try {
			buffer.set_size_discard( size );
			return;
		} catch(std::bad_alloc const &) {
			if (size < 256) throw;
			size >>= 1;
		}
	}
}

t_filesize file::g_transfer(stream_reader * p_src,stream_writer * p_dst,t_filesize p_bytes,abort_callback & p_abort) {
	pfc::array_t<t_uint8> temp;
	makeBuffer(temp, (t_size)pfc::min_t<t_filesize>(1024*1024*8,p_bytes));
	void* ptr = temp.get_ptr();
	t_filesize done = 0;
	while(done<p_bytes) {
		p_abort.check_e();
		t_size delta = (t_size)pfc::min_t<t_filesize>(temp.get_size(),p_bytes-done);
		delta = p_src->read(ptr,delta,p_abort);
		if (delta<=0) break;
		p_dst->write(ptr,delta,p_abort);
		done += delta;
	}
	return done;
}

void file::g_transfer_object(stream_reader * p_src,stream_writer * p_dst,t_filesize p_bytes,abort_callback & p_abort) {
	if (g_transfer(p_src,p_dst,p_bytes,p_abort) != p_bytes)
		throw exception_io_data_truncation();
}


void filesystem::g_get_canonical_path(const char * path,pfc::string_base & out)
{
	// TRACK_CALL_TEXT("filesystem::g_get_canonical_path");
	for (auto ptr : enumerate()) {
		if (ptr->get_canonical_path(path, out)) return;
	}
	//no one wants to process this, let's copy over
	out = path;
}

void filesystem::g_get_display_path(const char* path, pfc::string_base& out, filesystem::ptr& reuseMe) {

	if (reuseMe.is_valid() && reuseMe->is_our_path(path)) {
		if (!reuseMe->get_display_path(path, out)) {
			// should not get here
			out = path;
		}
	} else {
		if (!g_get_interface(reuseMe, path)) {
			out = path;
			return;
		}
		if (!reuseMe->get_display_path(path, out)) {
			// should not get here
			out = path;
		}
	}
}

void filesystem::g_get_display_path(const char * path,pfc::string_base & out)
{
	// TRACK_CALL_TEXT("filesystem::g_get_display_path");
	service_ptr_t<filesystem> ptr;
	if (!g_get_interface(ptr,path))
	{
		//no one wants to process this, let's copy over
		out = path;
	}
	else
	{
		if (!ptr->get_display_path(path,out))
			out = path;
	}
}

pfc::string8 filesystem::g_get_native_path( const char * path, abort_callback & a ) {
    pfc::string8 ret;
    g_get_native_path( path, ret, a);
    return ret;
}

bool filesystem::g_get_native_path( const char * path, pfc::string_base & out, abort_callback & a) {
    // Is proper file:// path?
    if (foobar2000_io::extract_native_path( path, out ) ) return true;

	{
		filesystem_v3::ptr fs;
		if (fs &= tryGet(path)) {
			auto n = fs->getNativePath(path, a);
			if (n.is_valid()) {
				out = n->c_str(); return true;
			}
		}
	}

    // Set anyway
    out = path;
    
    // Maybe just a file:// less local path? Check for other protocol markers
    // If no :// present, return true anyway
    return strstr( path, "://" ) == NULL;
}

filesystem::ptr filesystem::getLocalFS() {
	return get("file://dummy");
}

filesystem::ptr filesystem::tryGet(const char* path) {
	filesystem::ptr rv;
	g_get_interface(rv, path);
	return rv;
}

filesystem::ptr filesystem::g_get_interface(const char * path) {
	filesystem::ptr rv;
	if (!g_get_interface(rv, path)) throw exception_io_no_handler_for_path();
	return rv;

}

#define USE_FSCACHE 1
#if USE_FSCACHE
#include <unordered_map>

static pfc::readWriteLock fsCacheGuard;

typedef size_t protoHash_t;
static protoHash_t protoHash(const char * URL) {
	const char* delim = strstr(URL, "://");
	if (delim == nullptr) return 0;

	union {
		protoHash_t hash;
		char chars[sizeof(protoHash_t)];
	} u;
	u.hash = 0;
	unsigned i = 0;
	for (const char* walk = URL; walk != delim; ++walk) {
		char c = *walk;
		if (c > 0) c = pfc::ascii_tolower_lookup(c);
		u.chars[i] ^= c;
		i = (i + 1) % std::size(u.chars);
	}
	return u.hash;
}

// Do not use service_ptr in static objects, do not try to release them in static object destructor
static std::unordered_multimap< protoHash_t, filesystem* > fsCache;

static bool read_fs_cache(protoHash_t key, const char * path, filesystem::ptr& ret) {
	auto range = fsCache.equal_range(key);
	for (auto walk = range.first; walk != range.second; ++walk) {
		if (walk->second->is_our_path(path)) {
			ret = walk->second;
			return true;
		}
	}
	return false;
}

bool filesystem::g_get_interface(service_ptr_t<filesystem> & p_out,const char * path)
{
	PFC_ASSERT( path != nullptr );
	PFC_ASSERT( path[0] != 0 );

	const auto key = protoHash(path);

	{
		PFC_INSYNC_READ(fsCacheGuard);
		if (read_fs_cache(key, path, p_out)) return true;
	}

	for (auto ptr : enumerate()) {
		if (ptr->is_our_path(path)) {
			{
				PFC_INSYNC_WRITE(fsCacheGuard);
				filesystem::ptr dummy; // make sure it didn't just get added
				if (!read_fs_cache(key, path, dummy)) {
					auto addref = ptr;
					fsCache.insert({ key, addref.detach()});
				}
			}
			p_out = std::move(ptr);
			return true;
		}
	}
	return false;
}


#else
bool filesystem::g_get_interface(service_ptr_t<filesystem>& p_out, const char* path)
{
	PFC_ASSERT(path != nullptr);
	PFC_ASSERT(path[0] != 0);

	for (auto ptr : enumerate()) {
		if (ptr->is_our_path(path)) {
			p_out = std::move(ptr);
			return true;
		}
	}
	return false;
}

#endif

void filesystem::g_open(service_ptr_t<file> & p_out,const char * path,t_open_mode mode,abort_callback & p_abort)
{
	TRACK_CALL_TEXT("filesystem::g_open");
	g_get_interface(path)->open(p_out,path,mode,p_abort);
}


void filesystem::g_open_timeout(service_ptr_t<file> & p_out,const char * p_path,t_open_mode p_mode,double p_timeout,abort_callback & p_abort) {
	FB2K_RETRY_ON_SHARING_VIOLATION( g_open(p_out, p_path, p_mode, p_abort), p_abort, p_timeout);
}

bool filesystem::g_exists(const char * p_path,abort_callback & p_abort)
{
	t_filestats stats;
	bool dummy;
	try {
		g_get_stats(p_path,stats,dummy,p_abort);
	} catch(exception_io_not_found const &) {return false;}
	return true;
}

bool filesystem::g_exists_writeable(const char * p_path,abort_callback & p_abort)
{
	t_filestats stats;
	bool writeable;
	try {
		g_get_stats(p_path,stats,writeable,p_abort);
	} catch(exception_io_not_found const &) {return false;}
	return writeable;
}

void filesystem::g_remove(const char * p_path,abort_callback & p_abort) {
	g_get_interface(p_path)->remove(p_path,p_abort);
}

void filesystem::g_remove_timeout(const char * p_path,double p_timeout,abort_callback & p_abort) {
	FB2K_RETRY_FILE_MOVE( g_remove(p_path, p_abort), p_abort, p_timeout );
}

void filesystem::g_move_timeout(const char * p_src,const char * p_dst,double p_timeout,abort_callback & p_abort) {
	FB2K_RETRY_FILE_MOVE( g_move(p_src, p_dst, p_abort), p_abort, p_timeout );
}

void filesystem::g_copy_timeout(const char * p_src,const char * p_dst,double p_timeout,abort_callback & p_abort) {
	FB2K_RETRY_FILE_MOVE( g_copy(p_src, p_dst, p_abort), p_abort, p_timeout );
}

void filesystem::g_create_directory(const char * p_path,abort_callback & p_abort)
{
	g_get_interface(p_path)->create_directory(p_path,p_abort);
}

void filesystem::g_move(const char * src,const char * dst,abort_callback & p_abort) {
	for (auto ptr : enumerate()) {
		if (ptr->is_our_path(src) && ptr->is_our_path(dst)) {
			ptr->move(src, dst, p_abort);
			return;
		}
	}
	throw exception_io_no_handler_for_path();
}

void filesystem::g_link(const char * p_src,const char * p_dst,abort_callback & p_abort) {
	p_abort.check();
    pfc::string8 srcN, dstN;
    if (!extract_native_path(p_src, srcN) || !extract_native_path(p_dst, dstN)) throw exception_io_no_handler_for_path();
#ifdef _WIN32
    WIN32_IO_OP( CreateHardLink( pfc::stringcvt::string_os_from_utf8( dstN ), pfc::stringcvt::string_os_from_utf8( srcN ), NULL) );
#else
    NIX_IO_OP( symlink( srcN, dstN ) == 0 );
#endif
}

void filesystem::g_link_timeout(const char * p_src,const char * p_dst,double p_timeout,abort_callback & p_abort) {
	FB2K_RETRY_FILE_MOVE( g_link(p_src, p_dst, p_abort), p_abort, p_timeout );
}


void filesystem::g_list_directory(const char * p_path,directory_callback & p_out,abort_callback & p_abort)
{
	TRACK_CALL_TEXT("filesystem::g_list_directory");
	g_get_interface(p_path)->list_directory(p_path,p_out,p_abort);
}


static void path_pack_string(pfc::string_base & out,const char * src)
{
	out.add_char('|');
	out << (unsigned) strlen(src);
	out.add_char('|');
	out << src;
	out.add_char('|');
}

static int path_unpack_string(pfc::string_base & out,const char * src)
{
	int ptr=0;
	if (src[ptr++]!='|') return -1;
	int len = atoi(src+ptr);
	if (len<=0) return -1;
	while(src[ptr]!=0 && src[ptr]!='|') ptr++;
	if (src[ptr]!='|') return -1;
	ptr++;
	int start = ptr;
	while(ptr-start<len)
	{
		if (src[ptr]==0) return -1;
		ptr++;
	}
	if (src[ptr]!='|') return -1;
	out.set_string(&src[start],len);
	ptr++;	
	return ptr;
}


void filesystem::g_open_precache(service_ptr_t<file> & p_out,const char * p_path,abort_callback & p_abort) {
	service_ptr_t<filesystem> fs = g_get_interface(p_path);
	if (fs->is_remote(p_path)) throw exception_io_object_is_remote();
	fs->open(p_out,p_path,open_mode_read,p_abort);
}

bool filesystem::g_is_remote(const char * p_path) {
	return g_get_interface(p_path)->is_remote(p_path);
}

bool filesystem::g_is_recognized_and_remote(const char * p_path) {
	service_ptr_t<filesystem> fs;
	if (g_get_interface(fs,p_path)) return fs->is_remote(p_path);
	else return false;
}

bool filesystem::g_is_remote_or_unrecognized(const char * p_path) {
	service_ptr_t<filesystem> fs;
	if (g_get_interface(fs,p_path)) return fs->is_remote(p_path);
	else return true;
}

bool filesystem::g_relative_path_create(const char * file_path,const char * playlist_path,pfc::string_base & out)
{
	
	bool rv = false;
	service_ptr_t<filesystem> fs;

	if (g_get_interface(fs,file_path))
		rv = fs->relative_path_create(file_path,playlist_path,out);
	
	return rv;
}

bool filesystem::g_relative_path_parse(const char * relative_path,const char * playlist_path,pfc::string_base & out)
{
	for (auto ptr : enumerate()) {
		if (ptr->relative_path_parse(relative_path, playlist_path, out)) return true;
	}
	return false;
}

namespace {
	class archive_callback_lambda : public archive_callback {
	private:
		abort_callback& m_abort;
	public:
		bool is_aborting() const override { return m_abort.is_aborting(); }
		abort_callback_event get_abort_event() const override { return m_abort.get_abort_event(); }

		archive_callback_lambda(abort_callback& a) : m_abort(a) {}
		bool on_entry(archive*, const char* url, const t_filestats& p_stats, const service_ptr_t<file>& p_reader) override {
			f(url, p_stats, p_reader);
			return true;
		}

		archive::list_func_t f;
	};
}

void archive::archive_list(const char * path, file::ptr reader, list_func_t f, bool wantReaders, abort_callback& a ) {
	archive_callback_lambda cb(a);
	cb.f = f;
	this->archive_list(path, reader, cb, wantReaders);
}

bool archive::is_our_archive( const char * path ) {
	archive_v2::ptr v2;
	if ( v2 &= this ) return v2->is_our_archive( path );
	return true; // accept all files
}

void archive_impl::extract_filename_ext(const char * path, pfc::string_base & outFN) {
	pfc::string8 dummy, subpath;
	if (archive_impl::g_parse_unpack_path(path, dummy, subpath)) {
		outFN = pfc::filename_ext_v2( subpath );
	} else {
		PFC_ASSERT(!"???");
		filesystem_v3::extract_filename_ext(path, outFN);
	}
}

bool archive_impl::get_display_name_short(const char* in, pfc::string_base& out) {
	extract_filename_ext(in ,out);
	return true;
}

bool archive_impl::get_canonical_path(const char * path,pfc::string_base & out)
{
	if (is_our_path(path))
	{
		pfc::string8 archive,file,archive_canonical;
		if (g_parse_unpack_path(path,archive,file))
		{
			g_get_canonical_path(archive,archive_canonical);
			make_unpack_path(out,archive_canonical,file);

			return true;
		}
		else return false;
	}
	else return false;
}

bool archive_impl::is_our_path(const char * path)
{
	if (!g_is_unpack_path(path)) return false;
	const char * type = get_archive_type();
	path += 9;
	while(*type)
	{
		if (*type!=*path) return false;
		type++;
		path++;
	}
	if (*path!='|') return false;
	return true;
}

bool archive_impl::get_display_path(const char * path,pfc::string_base & out)
{
	pfc::string8 archive,file;
	if (g_parse_unpack_path(path,archive,file))
	{
		g_get_display_path(archive,out);
		out.add_string("|");
		out.add_string(file);
		return true;
	}
	else return false;
}

void archive_impl::open(service_ptr_t<file> & p_out,const char * path,t_open_mode mode, abort_callback & p_abort)
{
	if (mode != open_mode_read) throw exception_io_denied();
	pfc::string8 archive,file;
	if (!g_parse_unpack_path(path,archive,file)) throw exception_io_not_found();
	open_archive(p_out,archive,file,p_abort);
}


void archive_impl::remove(const char * path,abort_callback & p_abort) {
	(void)p_abort; (void)path;
    pfc::throw_exception_with_message< exception_io_denied> ("Cannot delete files within archives");
}

void archive_impl::move(const char * src,const char * dst,abort_callback & p_abort) {
	(void)p_abort; (void)src; (void)dst;
    pfc::throw_exception_with_message< exception_io_denied> ("Cannot move files within archives");
}

void archive_impl::move_overwrite(const char* src, const char* dst, abort_callback& abort) {
	(void)abort; (void)src; (void)dst;
    pfc::throw_exception_with_message< exception_io_denied> ("Cannot move files within archives");
}

bool archive_impl::is_remote(const char * src) {
	pfc::string8 archive,file;
	if (g_parse_unpack_path(src,archive,file)) return g_is_remote(archive);
	else throw exception_io_not_found();
}

bool archive_impl::relative_path_create(const char * file_path,const char * playlist_path,pfc::string_base & out) {
	pfc::string8 archive,file;
	if (g_parse_unpack_path(file_path,archive,file))
	{
		pfc::string8 archive_rel;
		if (g_relative_path_create(archive,playlist_path,archive_rel))
		{
			pfc::string8 out_path;
			make_unpack_path(out_path,archive_rel,file);
			out.set_string(out_path);
			return true;
		}
	}
	return false;
}

bool archive_impl::relative_path_parse(const char * relative_path,const char * playlist_path,pfc::string_base & out)
{
	if (!is_our_path(relative_path)) return false;
	pfc::string8 archive_rel,file;
	if (g_parse_unpack_path(relative_path,archive_rel,file))
	{
		pfc::string8 archive;
		if (g_relative_path_parse(archive_rel,playlist_path,archive))
		{
			pfc::string8 out_path;
			make_unpack_path(out_path,archive,file);
			out.set_string(out_path);
			return true;
		}
	}
	return false;
}

bool archive_impl::g_parse_unpack_path_ex(const char * path,pfc::string_base & archive,pfc::string_base & file, pfc::string_base & type) {
	PFC_ASSERT( g_is_unpack_path(path) );
	const char * base = path + unpack_prefix_len; // strstr(path, "//");
	const char * split = strchr(path,'|');
	if (base == NULL || split == NULL || base > split) return false;
	// base += 2;
	type.set_string( base, split - base );
	int delta = path_unpack_string(archive,split);
	if (delta<0) return false;
	split += delta;
	file = split;
	return true;
}
bool archive_impl::g_parse_unpack_path(const char * path,pfc::string_base & archive,pfc::string_base & file) {
	PFC_ASSERT( g_is_unpack_path(path) );
	path  = strchr(path,'|');
	if (!path) return false;
	int delta = path_unpack_string(archive,path);
	if (delta<0) return false;
	path += delta;
	file = path;
	return true;
}

bool archive_impl::g_is_unpack_path(const char * path) {
	return strncmp(path,unpack_prefix,unpack_prefix_len) == 0;
}

void archive_impl::g_make_unpack_path(pfc::string_base & path,const char * archive,const char * file,const char * name)
{
	path = unpack_prefix;
	path += name;
	path_pack_string(path,archive);
	path += file;
}

void archive_impl::make_unpack_path(pfc::string_base & path,const char * archive,const char * file) {g_make_unpack_path(path,archive,file,get_archive_type());}

fb2k::arrayRef archive_impl::archive_list_v4( fsItemFilePtr item, file::ptr readerOptional, abort_callback & a ) {
    
    const auto baseStats = item->getStatsOpportunist();
    PFC_ASSERT( ! baseStats.is_folder() );
    auto ret = fb2k::arrayMutable::arrayWithCapacity(256);
    
    auto reader = readerOptional;
    if ( reader.is_empty() ) reader = item->openRead(a);
    try {
        this->archive_list( item->canonicalPath()->c_str(), reader, [&] ( const char * URL, t_filestats const & stats, file::ptr ) {
            t_filestats2 stats2 = t_filestats2::from_legacy( stats );
            stats2.set_file(); stats2.set_remote( baseStats.is_remote() ); stats2.set_readonly(true);
            archive * blah = this; // multi inheritance fix, more than one path to filesystem which has makeItemFileStd()
            ret->add(blah->makeItemFileStd(URL, stats2));
        }, false, a);
    } catch( exception_io_data const & ) {
        if ( ret->count() == 0 ) throw;
    }
    return ret->makeConst();
    
}

namespace {

	class directory_callback_isempty : public directory_callback
	{
		bool m_isempty;
	public:
		directory_callback_isempty() : m_isempty(true) {}
		bool on_entry(filesystem *,abort_callback &,const char *,bool,const t_filestats &) override
		{
			m_isempty = false;
			return false;
		}
		bool isempty() {return m_isempty;}
	};

	class directory_callback_dummy : public directory_callback
	{
	public:
		bool on_entry(filesystem *,abort_callback &,const char *,bool,const t_filestats &) override {return false;}
	};

}

bool filesystem::g_is_empty_directory(const char * path,abort_callback & p_abort)
{
	directory_callback_isempty callback;
	try {
		g_list_directory(path,callback,p_abort);
	} catch(exception_io const &) {return false;}
	return callback.isempty();
}

bool filesystem::g_is_valid_directory(const char * path,abort_callback & p_abort) {
	if ( path == NULL || path[0] == 0 ) return false;

	return get(path)->directory_exists( path, p_abort );
}

bool directory_callback_impl::on_entry(filesystem * owner,abort_callback & p_abort,const char * url,bool is_subdirectory,const t_filestats & p_stats) {
	p_abort.check_e();
	if (is_subdirectory) {
		if (m_recur) {
			try {
				owner->list_directory(url,*this,p_abort);
			} catch(exception_io const &) {}
		}
	} else {
		m_data.add_item(pfc::rcnew_t<t_entry>(url,p_stats));
	}
	return true;
}

namespace {
	class directory_callback_impl_copy : public directory_callback
	{
	public:
		directory_callback_impl_copy(const char * p_target, filesystem::ptr fs) : m_fs(fs)
		{
			m_target = p_target;
			m_target.fix_dir_separator();
		}

		bool on_entry(filesystem * owner,abort_callback & p_abort,const char * url,bool is_subdirectory,const t_filestats & p_stats) override {
			(void)p_stats;
			const char * fn = url + pfc::scan_filename(url);
			t_size truncat = m_target.length();
			m_target += fn;
			if (is_subdirectory) {
				try {
					m_fs->create_directory(m_target,p_abort);
				} catch(exception_io_already_exists const &) {}
				m_target.end_with_slash();
				owner->list_directory(url,*this,p_abort);
			} else {
				_copy(url, m_target, owner, p_abort);
			}
			m_target.truncate(truncat);
			return true;
		}
		void _copy(const char * src, const char * dst, filesystem * srcFS, abort_callback & p_abort) {
			service_ptr_t<file> r_src,r_dst;
			t_filesize size;

			srcFS->open(r_src,src,filesystem::open_mode_read,p_abort);
			size = r_src->get_size_ex(p_abort);
			m_fs->open(r_dst,dst,filesystem::open_mode_write_new,p_abort);
	
			if (size > 0) {
				try {
					file::g_transfer_object(r_src,r_dst,size,p_abort);
					file::g_copy_timestamps(r_src, r_dst, p_abort);
				} catch(...) {
					r_dst.release();
                    try {m_fs->remove(dst,fb2k::noAbort);} catch(...) {}
					throw;
				}
			}
		}
	private:
		pfc::string8_fastalloc m_target;
		filesystem::ptr m_fs;
	};
}

file::ptr filesystem::openEx(const char * path, filesystem::t_open_mode mode, abort_callback & abort, double timeout) {
	file::ptr f;
	retryOnSharingViolation([&] {
		this->open(f, path, mode, abort);
	}, timeout, abort);
	return f;
}

file::ptr filesystem::openRead(const char * path, abort_callback & abort, double timeout) {
	return this->openEx(path, open_mode_read, abort, timeout);
}

file::ptr filesystem::openWriteExisting(const char * path, abort_callback & abort, double timeout) {
	return this->openEx(path, open_mode_write_existing, abort, timeout);
}

file::ptr filesystem::openWriteNew(const char * path, abort_callback & abort, double timeout) {
	return this->openEx( path, open_mode_write_new, abort, timeout );
}

void filesystem::remove_(const char* path, abort_callback& a, double timeout) {
	retryOnSharingViolation(timeout, a, [&] {
		this->remove(path, a);
	});
}

void filesystem::copy_directory_contents(const char* p_src, const char* p_dst, abort_callback& p_abort) {
	directory_callback_impl_copy cb(p_dst, this);
	list_directory(p_src, cb, p_abort);
}

void filesystem::copy_directory(const char * src, const char * dst, abort_callback & p_abort) {
	try {
		this->create_directory( dst, p_abort );
	} catch(exception_io_already_exists const &) {}
	this->copy_directory_contents(src, dst, p_abort);
}

void filesystem::g_copy_directory(const char * src,const char * dst,abort_callback & p_abort) {
	filesystem::ptr dstFS = filesystem::g_get_interface(dst);
	try {
		dstFS->create_directory( dst, p_abort );
	} catch(exception_io_already_exists const &) {}
	directory_callback_impl_copy cb(dst, dstFS);
	g_list_directory(src,cb,p_abort);
}

void filesystem::g_copy(const char * src,const char * dst,abort_callback & p_abort) {
	service_ptr_t<file> r_src,r_dst;
	t_filesize size;

	g_open(r_src,src,open_mode_read,p_abort);
	size = r_src->get_size_ex(p_abort);
	g_open(r_dst,dst,open_mode_write_new,p_abort);
	
	if (size > 0) {
		try {
			file::g_transfer_object(r_src,r_dst,size,p_abort);
		} catch(...) {
			r_dst.release();
			try {g_remove(dst,fb2k::noAbort);} catch(...) {}
			throw;
		}
	}

	try {
		file::g_copy_timestamps(r_src, r_dst, p_abort);
	} catch (exception_io const &) {}
}

void stream_reader::read_object(void * p_buffer,t_size p_bytes,abort_callback & p_abort) {
	if (read(p_buffer,p_bytes,p_abort) != p_bytes) throw exception_io_data_truncation();
}

t_filestats file::get_stats(abort_callback & p_abort)
{
	t_filestats temp;
	temp.m_size = get_size(p_abort);
	temp.m_timestamp = get_timestamp(p_abort);
	return temp;
}

t_filesize stream_reader::skip(t_filesize p_bytes,abort_callback & p_abort)
{
	t_uint8 temp[256];
	t_filesize todo = p_bytes, done = 0;
	while(todo > 0) {
		t_size delta,deltadone;
		delta = sizeof(temp);
		if (delta > todo) delta = (t_size) todo;
		deltadone = read(temp,delta,p_abort);
		done += deltadone;
		todo -= deltadone;
		if (deltadone < delta) break;
	}
	return done;
}

void stream_reader::skip_object(t_filesize p_bytes,abort_callback & p_abort) {
	if (skip(p_bytes,p_abort) != p_bytes) throw exception_io_data_truncation();
}

void filesystem::g_open_write_new(service_ptr_t<file> & p_out,const char * p_path,abort_callback & p_abort) {
	g_open(p_out,p_path,open_mode_write_new,p_abort);
}
void file::g_transfer_file(const service_ptr_t<file> & p_from,const service_ptr_t<file> & p_to,abort_callback & p_abort) {
	t_filesize length = p_from->get_size(p_abort);
	p_from->reopen( p_abort );
//	p_from->seek(0,p_abort);
	p_to->seek(0,p_abort);
	p_to->set_eof(p_abort);
	if (length == filesize_invalid) {
		g_transfer(p_from, p_to, filesize_invalid, p_abort);
	} else if (length > 0) {
		g_transfer_object(p_from,p_to,length,p_abort);
	}
}

void filesystem::g_open_temp(service_ptr_t<file> & p_out,abort_callback & p_abort) {
	g_open(p_out,"tempfile://",open_mode_write_new,p_abort);
}

void filesystem::g_open_tempmem(service_ptr_t<file> & p_out,abort_callback & p_abort) {
	g_open(p_out,"tempmem://",open_mode_write_new,p_abort);
}

file::ptr filesystem::g_open_tempmem() {
	file::ptr f; g_open_tempmem(f, fb2k::noAbort); return f;
}

void archive_impl::list_directory(const char * p_path,directory_callback & p_out,abort_callback & p_abort) {
	(void)p_path; (void)p_out; (void)p_abort;
	throw exception_io_not_found();
}

void archive_impl::list_directory_ex(const char* p_path, directory_callback& p_out, unsigned listMode, abort_callback& p_abort) {
	(void)p_path; (void)p_out; (void)listMode; (void)p_abort;
	throw exception_io_not_found();
}

void archive_impl::list_directory_v3(const char* path, directory_callback_v3& callback, unsigned listMode, abort_callback& p_abort) {
	(void)path; (void)callback; (void)listMode; (void)p_abort;
	throw exception_io_not_found();
}

void archive_impl::create_directory(const char *,abort_callback &) {
	throw exception_io_denied();
}

void archive_impl::make_directory(const char*, abort_callback&, bool*) {
	throw exception_io_denied();
}

void filesystem::g_get_stats(const char * p_path,t_filestats & p_stats,bool & p_is_writeable,abort_callback & p_abort) {
	TRACK_CALL_TEXT("filesystem::g_get_stats");
	return g_get_interface(p_path)->get_stats(p_path,p_stats,p_is_writeable,p_abort);
}

// Due to multi inheritance from archive and filesystem_v3, filesystem_v3's get_stats() isn't overriding archive's method. Fix that here.
void archive_impl::get_stats(const char* p_path, t_filestats& p_stats, bool& p_is_writeable, abort_callback& p_abort) {
	filesystem_v3::get_stats(p_path, p_stats, p_is_writeable, p_abort);
}

t_filestats2 archive_impl::get_stats2(const char * p_path,unsigned s2flags,abort_callback & p_abort) {
	pfc::string8 archive,file;
	if (g_parse_unpack_path(p_path,archive,file)) {
		if (g_is_remote(archive)) throw exception_io_object_is_remote();
		t_filestats2 ret = get_stats2_in_archive(archive,file,s2flags,p_abort);
		ret.set_readonly(true);
		ret.set_folder(false);
		ret.set_remote(false);
		return ret;
	}
	else throw exception_io_not_found();
}


bool file::is_eof(abort_callback & p_abort) {
	t_filesize position,size;
	position = get_position(p_abort);
	size = get_size(p_abort);
	if (size == filesize_invalid) return false;
	return position >= size;
}

t_filetimestamp foobar2000_io::filetimestamp_from_system_timer()
{
    return pfc::fileTimeNow();
}

void stream_reader::read_string_ex(pfc::string_base & p_out,t_size p_bytes,abort_callback & p_abort) {
	const t_size expBase = 64*1024;
	if (p_bytes > expBase) {
		pfc::array_t<char> temp;
		t_size allocWalk = expBase;
		t_size done = 0;
		for(;;) {
			const t_size target = pfc::min_t(allocWalk, p_bytes);
			temp.set_size(target);
			read_object(temp.get_ptr() + done, target - done, p_abort);
			if (target == p_bytes) break;
			done = target;
			allocWalk <<= 1;
		}
		p_out.set_string(temp.get_ptr(), p_bytes);
	} else {
		pfc::string_buffer buf(p_out, p_bytes);
		read_object(buf.get_ptr(),p_bytes,p_abort);
	}
}
void stream_reader::read_string(pfc::string_base & p_out,abort_callback & p_abort)
{
	t_uint32 length;
	read_lendian_t(length,p_abort);
	read_string_ex(p_out,length,p_abort);
}

void stream_reader::read_string_raw(pfc::string_base & p_out,abort_callback & p_abort, size_t sanity) {
	enum {delta = 1024};
	char buffer[delta];
	p_out.reset();
	size_t didRead = 0;
	for(;;) {
		auto delta_done = read(buffer,delta,p_abort);
		p_out.add_string(buffer,delta_done);
		if (delta_done < delta) break;
		didRead += delta;
		if (didRead > sanity) throw exception_io_data();
	}
}

void stream_writer::write_string(const char * p_string,t_size p_len,abort_callback & p_abort) {
	t_uint32 len = pfc::downcast_guarded<t_uint32>(pfc::strlen_max(p_string,p_len));
	write_lendian_t(len,p_abort);
	write_object(p_string,len,p_abort);
}

void stream_writer::write_string(const char * p_string,abort_callback & p_abort) {
	write_string(p_string,SIZE_MAX,p_abort);
}

void stream_writer::write_string_raw(const char * p_string,abort_callback & p_abort) {
	write_object(p_string,strlen(p_string),p_abort);
}

void file::truncate(t_uint64 p_position,abort_callback & p_abort) {
	if (p_position < get_size(p_abort)) resize(p_position,p_abort);
}


#ifdef _WIN32
namespace {
	//rare/weird win32 errors that didn't make it to the main API
	PFC_DECLARE_EXCEPTION(exception_io_device_not_ready,		exception_io,"Device not ready");
	PFC_DECLARE_EXCEPTION(exception_io_invalid_drive,			exception_io_not_found,"Drive not found");
	PFC_DECLARE_EXCEPTION(exception_io_win32,					exception_io,"Generic win32 I/O error");
	PFC_DECLARE_EXCEPTION(exception_io_buffer_overflow,			exception_io,"The file name is too long");
	PFC_DECLARE_EXCEPTION(exception_io_invalid_path_syntax,		exception_io,"Invalid path syntax");

	class exception_io_win32_ex : public exception_io_win32 {
	public:
		static pfc::string8 format(DWORD code) {
			pfc::string8 ret;
			ret << "I/O error (win32 ";
			if (code & 0x80000000) {
				ret << "0x" << pfc::format_hex(code, 8);
			} else {
				ret << "#" << (uint32_t)code;
			}
			ret << ")";
			return ret;
		}
		exception_io_win32_ex(DWORD p_code) : m_msg(format(p_code)) {}
		exception_io_win32_ex(const exception_io_win32_ex & p_other) {*this = p_other;}
		const char * what() const throw() {return m_msg;}
	private:
		pfc::string8 m_msg;
	};
}

PFC_NORETURN void foobar2000_io::win32_file_write_failure(DWORD p_code, const char * path) {
	if (p_code == ERROR_ACCESS_DENIED) {
		const DWORD attr = uGetFileAttributes(path);
		if (attr != ~0 && (attr & FILE_ATTRIBUTE_READONLY) != 0) throw exception_io_denied_readonly();
	}
	exception_io_from_win32(p_code);
}

PFC_NORETURN void foobar2000_io::exception_io_from_win32(DWORD p_code) {
#if PFC_DEBUG
	PFC_DEBUGLOG << "exception_io_from_win32: " << p_code;
#endif
	//pfc::string_fixed_t<32> debugMsg; debugMsg << "Win32 I/O error #" << (t_uint32)p_code;
	//TRACK_CALL_TEXT(debugMsg);
	switch(p_code) {
	case ERROR_ALREADY_EXISTS:
	case ERROR_FILE_EXISTS:
		throw exception_io_already_exists();
	case ERROR_NETWORK_ACCESS_DENIED:
	case ERROR_ACCESS_DENIED:
		throw exception_io_denied();
	case ERROR_WRITE_PROTECT:
		throw exception_io_write_protected();
	case ERROR_BUSY:
	case ERROR_PATH_BUSY:
	case ERROR_SHARING_VIOLATION:
	case ERROR_LOCK_VIOLATION:
		throw exception_io_sharing_violation();
	case ERROR_HANDLE_DISK_FULL:
	case ERROR_DISK_FULL:
		throw exception_io_device_full();
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
		throw exception_io_not_found();
	case ERROR_BROKEN_PIPE:
	case ERROR_NO_DATA:
		throw exception_io_no_data();
	case ERROR_NETWORK_UNREACHABLE:
	case ERROR_NETNAME_DELETED:
		throw exception_io_network_not_reachable();
	case ERROR_NOT_READY:
		throw exception_io_device_not_ready();
	case ERROR_NO_SUCH_DEVICE:
	case ERROR_INVALID_DRIVE:
		throw exception_io_invalid_drive();
	case ERROR_CRC:
	case ERROR_FILE_CORRUPT:
	case ERROR_DISK_CORRUPT:
		throw exception_io_file_corrupted();
	case ERROR_BUFFER_OVERFLOW:
		throw exception_io_buffer_overflow();
	case ERROR_DISK_CHANGE:
		throw exception_io_disk_change();
	case ERROR_DIR_NOT_EMPTY:
		throw exception_io_directory_not_empty();
	case ERROR_INVALID_NAME:
		throw exception_io_invalid_path_syntax();
	case ERROR_NO_SYSTEM_RESOURCES:
	case ERROR_NONPAGED_SYSTEM_RESOURCES:
	case ERROR_PAGED_SYSTEM_RESOURCES:
	case ERROR_WORKING_SET_QUOTA:
	case ERROR_PAGEFILE_QUOTA:
	case ERROR_COMMITMENT_LIMIT:
		throw exception_io("Insufficient system resources");
	case ERROR_IO_DEVICE:
		throw exception_io("Device error");
	case ERROR_BAD_NETPATH:
		// known to be inflicted by momentary net connectivity issues - NOT the same as exception_io_not_found
		throw exception_io_network_not_reachable("Network path not found");
#if FB2K_SUPPORT_TRANSACTED_FILESYSTEM
	case ERROR_TRANSACTIONAL_OPEN_NOT_ALLOWED:
	case ERROR_TRANSACTIONS_UNSUPPORTED_REMOTE:
	case ERROR_RM_NOT_ACTIVE:
	case ERROR_RM_METADATA_CORRUPT:
	case ERROR_DIRECTORY_NOT_RM:
		throw exception_io_transactions_unsupported();
	case ERROR_TRANSACTIONAL_CONFLICT:
		throw exception_io_transactional_conflict();
	case ERROR_TRANSACTION_ALREADY_ABORTED:
		throw exception_io_transaction_aborted();
	case ERROR_EFS_NOT_ALLOWED_IN_TRANSACTION:
		throw exception_io("Transacted updates of encrypted content are not supported");
#endif // FB2K_SUPPORT_TRANSACTED_FILESYSTEM
	case ERROR_UNEXP_NET_ERR:
		// QNAP threw this when messing with very long file paths and concurrent conversion, probably SMB daemon crashed
		throw exception_io_network_not_reachable("Unexpected network error");
	case ERROR_NOT_SAME_DEVICE:
		throw exception_io("Source and destination must be on the same device");
	case 0x80310000:
		throw exception_io("Drive locked by BitLocker");
	case ERROR_INVALID_FUNCTION:
		// Happens when trying to link files on FAT32 etc
		throw exception_io_unsupported_feature();
#if 0
	case ERROR_BAD_LENGTH:
		FB2K_BugCheckEx("ERROR_BAD_LENGTH");
#endif
	default:
		throw exception_io_win32_ex(p_code);
	}
}
#else
PFC_NORETURN void foobar2000_io::exception_io_from_nix(int code) {
    switch(code) {
        case EPERM:
        case EACCES:
            throw exception_io_denied();
        case ENOENT:
        case ENODEV:
            throw exception_io_not_found();
        case EIO:
            pfc::throw_exception_with_message<exception_io>("Generic I/O error (EIO)");
        case EBUSY:
            throw exception_io_sharing_violation();
        case EEXIST:
            throw exception_io_already_exists();
        case ENOSPC:
            throw exception_io_device_full();
        case EROFS:
            throw exception_io_denied_readonly();
        case ENOTEMPTY:
            throw exception_io_directory_not_empty();
        case ESPIPE:
            // Should not actually get here
            PFC_ASSERT(!"Trying to seek a nonseekable stream");
            throw exception_io_object_not_seekable();
        case ENOTDIR:
            throw exception_io_not_directory();
        case ENAMETOOLONG:
            pfc::throw_exception_with_message<exception_io>("Name too long");
        default:
            pfc::throw_exception_with_message< exception_io>( PFC_string_formatter() << "Unknown I/O error (#" << code << ")");
    }
}
void nix_pre_io_op() {
    errno = 0;
}
PFC_NORETURN void foobar2000_io::nix_io_op_fail() {
    exception_io_from_nix( errno );
}
#endif

t_filesize file::get_size_ex(abort_callback & p_abort) {
	t_filesize temp = get_size(p_abort);
	if (temp == filesize_invalid) throw exception_io_no_length();
	return temp;
}

void file::ensure_local() {
	if (is_remote()) throw exception_io_object_is_remote();
}

void file::ensure_seekable() {
	if (!can_seek()) throw exception_io_object_not_seekable();
}

bool filesystem::g_is_recognized_path(const char * p_path) {
    filesystem::ptr obj;
	return g_get_interface(obj,p_path);
}

t_filesize file::get_remaining(abort_callback & p_abort) {
	t_filesize length = get_size_ex(p_abort);
	t_filesize position = get_position(p_abort);
	pfc::dynamic_assert(position <= length);
	return length - position;
}

bool file::probe_remaining_ex(t_filesize bytes, abort_callback& p_abort) {
	t_filesize length = get_size(p_abort);
	if (length != filesize_invalid) {
		t_filesize remaining = length - get_position(p_abort);
		if (remaining < bytes) return false;
	}
	return true;
}

void file::probe_remaining(t_filesize bytes, abort_callback & p_abort) {
	if (!probe_remaining_ex(bytes, p_abort)) throw exception_io_data_truncation();
}

t_filesize file::g_transfer(service_ptr_t<file> p_src,service_ptr_t<file> p_dst,t_filesize p_bytes,abort_callback & p_abort) {
	return g_transfer(pfc::implicit_cast<stream_reader*>(p_src.get_ptr()),pfc::implicit_cast<stream_writer*>(p_dst.get_ptr()),p_bytes,p_abort);
}

void file::g_transfer_object(service_ptr_t<file> p_src,service_ptr_t<file> p_dst,t_filesize p_bytes,abort_callback & p_abort) {
	if (p_bytes > 1024) /* don't bother on small objects */ 
	{
		t_filesize srcFileSize = p_src->get_size(p_abort); // detect truncation
		if (srcFileSize != ~0) {
			t_filesize remaining = srcFileSize - p_src->get_position(p_abort);
			if (p_bytes > remaining) throw exception_io_data_truncation();
		}

		t_filesize oldsize = p_dst->get_size(p_abort); // pre-resize the target file
		if (oldsize != filesize_invalid) {
			t_filesize newpos = p_dst->get_position(p_abort) + p_bytes;
			if (newpos > oldsize) p_dst->resize(newpos ,p_abort);
		}

	}
	g_transfer_object(pfc::implicit_cast<stream_reader*>(p_src.get_ptr()),pfc::implicit_cast<stream_writer*>(p_dst.get_ptr()),p_bytes,p_abort);
}


void foobar2000_io::generate_temp_location_for_file(pfc::string_base & p_out, const char * p_origpath,const char * p_extension,const char * p_magic) {
	hasher_md5_result hash;
	{
		auto hasher = hasher_md5::get();
		hasher_md5_state state;
		hasher->initialize(state);
		hasher->process(state,p_origpath,strlen(p_origpath));
		hasher->process(state,p_extension,strlen(p_extension));
		hasher->process(state,p_magic,strlen(p_magic));
		hash = hasher->get_result(state);
	}

	p_out = p_origpath;
	p_out.truncate(p_out.scan_filename());
	p_out += "temp-";
	p_out += pfc::format_hexdump(hash.m_data,sizeof(hash.m_data),"");
	p_out += ".";
	p_out += p_extension;
}

t_filesize file::skip_seek(t_filesize p_bytes,abort_callback & p_abort) {
	const t_filesize size = get_size(p_abort);
	if (size != filesize_invalid) {
		const t_filesize position = get_position(p_abort);
		const t_filesize toskip = pfc::min_t( p_bytes, size - position );
		seek(position + toskip,p_abort);
		return toskip;
	} else {
		this->seek_ex( p_bytes, seek_from_current, p_abort );
		return p_bytes;
	}
}

t_filesize file::skip(t_filesize p_bytes,abort_callback & p_abort) {
	if (p_bytes > 1024 && can_seek()) {
		const t_filesize size = get_size(p_abort);
		if (size != filesize_invalid) {
			const t_filesize position = get_position(p_abort);
			const t_filesize toskip = pfc::min_t( p_bytes, size - position );
			seek(position + toskip,p_abort);
			return toskip;
		}
	}
	return stream_reader::skip(p_bytes,p_abort);
}

bool foobar2000_io::is_native_filesystem( const char * p_fspath ) {
	return _extract_native_path_ptr( p_fspath );
}

bool foobar2000_io::_extract_native_path_ptr(const char * & p_fspath) {
	static const char header[] = "file://"; static const t_size headerLen = 7;
	if (strncmp(p_fspath,header,headerLen) != 0) return false;
	p_fspath += headerLen;
	return true;
}
bool foobar2000_io::extract_native_path(const char * p_fspath,pfc::string_base & p_native) {
	if (strstr(p_fspath, "://") != nullptr) {
		if (!_extract_native_path_ptr(p_fspath)) return false;
	}
	p_native = p_fspath;
#ifndef _WIN32
	expandHomeDir( p_native );
#endif
	return true;
}

bool foobar2000_io::extract_native_path_ex(const char * p_fspath, pfc::string_base & p_native) {
	if (!_extract_native_path_ptr(p_fspath)) return false;
	if (p_fspath[0] != '\\' || p_fspath[1] != '\\') {
		p_native = "\\\\?\\";
		p_native += p_fspath;
	} else {
		p_native = p_fspath;
	}
	return true;
}

static bool extract_native_path_fsv3(const char* in, pfc::string_base& out, abort_callback& a) {
	if (foobar2000_io::extract_native_path(in, out)) return true;
	filesystem_v3::ptr v3;
	if (v3 &= filesystem::tryGet(in)) {
		auto n = v3->getNativePath(in, a);
		if ( n.is_valid() ) {
			out = n->c_str(); return true;
		}
	}
	return false;
}

bool foobar2000_io::extract_native_path_archive_aware_ex(const char* in, pfc::string_base& out, abort_callback& a) {
	if (extract_native_path_fsv3(in, out, a)) return true;
	
	if (archive_impl::g_is_unpack_path(in)) {
		pfc::string8 arc, dummy;
		if (archive_impl::g_parse_unpack_path(in, arc, dummy)) {
			return extract_native_path_fsv3(arc, out, a);
		}
	}
	return false;
}

bool foobar2000_io::extract_native_path_archive_aware(const char * in, pfc::string_base & out) {
	if (foobar2000_io::extract_native_path(in, out)) return true;
	if (archive_impl::g_is_unpack_path(in)) {
		pfc::string8 arc, dummy;
		if (archive_impl::g_parse_unpack_path(in, arc, dummy)) {
			return foobar2000_io::extract_native_path(arc, out);
		}
	}
	return false;
}

pfc::string stream_reader::read_string(abort_callback & p_abort) {
	t_uint32 len;
	read_lendian_t(len,p_abort);
	return read_string_ex(len,p_abort);
}
pfc::string stream_reader::read_string_ex(t_size p_len,abort_callback & p_abort) {
	pfc::string temp;
	this->read_string_ex(temp, p_len, p_abort);
	return temp;
}


void filesystem::remove_directory_content(const char * path, abort_callback & abort) {
	class myCallback : public directory_callback {
	public:
		bool on_entry(filesystem * p_owner,abort_callback & p_abort,const char * p_url,bool p_is_subdirectory,const t_filestats &) {
			if (p_is_subdirectory) p_owner->list_directory(p_url, *this, p_abort);
			try {
				p_owner->remove(p_url, p_abort);
			} catch(exception_io_not_found const &) {}
			return true;
		}
	};
	myCallback cb;
	list_directory(path, cb, abort);
}
void filesystem::remove_object_recur(const char * path, abort_callback & abort) {
	filesystem_v3::ptr v3;
	if (v3 &= this) {
		// the new way
		// fsItemFolder may implement removeRecur() more efficiently
		auto item = v3->findItem(path, abort);
		fsItemFolderPtr folder;
		if (folder &= item) folder->removeRecur(abort);
		else item->remove(abort);
		return;
	}

	// the classic way
	try {
		remove_directory_content(path, abort);
	} catch(exception_io_not_found const &) {}
	remove(path, abort);

}

void filesystem::g_remove_object_recur_timeout(const char * path, double timeout, abort_callback & abort) {
	FB2K_RETRY_FILE_MOVE( g_remove_object_recur(path, abort), abort, timeout );
}

void filesystem::g_remove_object_recur(const char * path, abort_callback & abort) {
	g_get_interface(path)->remove_object_recur(path, abort);
}

void foobar2000_io::purgeOldFiles(const char * directory, t_filetimestamp period, abort_callback & abort) {

	class myCallback : public directory_callback {
	public:
		myCallback(t_filetimestamp period) : m_base(filetimestamp_from_system_timer() - period) {}
		bool on_entry(filesystem *,abort_callback & p_abort,const char * p_url,bool p_is_subdirectory,const t_filestats & p_stats) {
			if (!p_is_subdirectory && p_stats.m_timestamp < m_base) {
				try {
					filesystem::g_remove_timeout(p_url, 1, p_abort);
				} catch(exception_io_not_found const &) {}
			}
			return true;
		}
	private:
		const t_filetimestamp m_base;
	};

	myCallback cb(period);
	filesystem::g_list_directory(directory, cb, abort);
}

void stream_reader::read_string_nullterm( pfc::string_base & out, abort_callback & abort ) {
	enum { bufCount = 256 };
	char buffer[bufCount];
	out.reset();
	size_t w = 0;
	for(;;) {
		char & c = buffer[w];
		this->read_object( &c, 1, abort );
		if (c == 0) {
			out.add_string( buffer, w ); break;
		}
		if (++w == bufCount ) {
			out.add_string( buffer, bufCount ); w = 0;
		}
	}
}

t_filesize stream_reader::skip_till_eof(abort_callback & abort) {
	t_filesize atOnce = 1024 * 1024;
	t_filesize done = 0;
	for (;; ) {
		abort.check();
		t_filesize did = this->skip(atOnce, abort);
		done += did;
		if (did != atOnce) break;
	}
	return done;
}

uint8_t stream_reader::read_byte( abort_callback & abort ) {
	uint8_t b;
	read_object(&b, 1, abort );
	return b;
}

bool foobar2000_io::matchContentType(const char * fullString, const char * ourType) {
    t_size lim = pfc::string_find_first(fullString, ';');
    if (lim != ~0) {
        while(lim > 0 && fullString[lim-1] == ' ') --lim;
    }
    return pfc::stricmp_ascii_ex(fullString,lim, ourType, SIZE_MAX) == 0;
}

const char * foobar2000_io::contentTypeFromExtension( const char * ext ) {
    if ( pfc::stringEqualsI_ascii( ext, "mp3" ) ) return "audio/mpeg";
    if ( pfc::stringEqualsI_ascii( ext, "flac" ) ) return "audio/flac";
    if ( pfc::stringEqualsI_ascii( ext, "mp4" ) ) return "application/mp4"; // We don't know if it's audio-only or other.
    if ( pfc::stringEqualsI_ascii( ext, "m4a" ) ) return "audio/mp4";
    if ( pfc::stringEqualsI_ascii( ext, "mpc" ) ) return "audio/musepack";
    if ( pfc::stringEqualsI_ascii( ext, "ogg" ) ) return "audio/ogg";
    if ( pfc::stringEqualsI_ascii( ext, "opus" ) ) return "audio/opus";
    if ( pfc::stringEqualsI_ascii( ext, "wav" ) ) return "audio/vnd.wave";
    if ( pfc::stringEqualsI_ascii( ext, "wv" ) ) return "audio/wavpack";
    if ( pfc::stringEqualsI_ascii( ext, "txt" ) || pfc::stringEqualsI_ascii( ext, "cue" ) || pfc::stringEqualsI_ascii( ext, "log" ) ) return "text/plain";
    return "application/binary";
}

const char * foobar2000_io::extensionFromContentType( const char * contentType ) {
    if (matchContentType_MP3( contentType )) return "mp3";
    if (matchContentType_FLAC( contentType )) return "flac";
    if (matchContentType_MP4audio( contentType)) return "m4a";
    if (matchContentType_MP4( contentType)) return "mp4";
    if (matchContentType_Musepack( contentType )) return "mpc";
    if (matchContentType_Ogg( contentType )) return "ogg";
    if (matchContentType_Opus( contentType )) return "opus";
    if (matchContentType_WAV( contentType )) return "wav";
    if (matchContentType_WavPack( contentType )) return "wv";
    if (matchContentType(contentType, "image/jpeg")) return "jpg";
    if (matchContentType(contentType, "image/png")) return "png";
    return "";
}

bool foobar2000_io::matchContentType_MP3( const char * type) {
    return matchContentType(type,"audio/mp3") || matchContentType(type,"audio/mpeg") || matchContentType(type,"audio/mpg") || matchContentType(type,"audio/x-mp3") || matchContentType(type,"audio/x-mpeg") || matchContentType(type,"audio/x-mpg");
}
bool foobar2000_io::matchContentType_MP4( const char * type ) {
    return matchContentType_MP4audio(type)
    || matchContentType(type, "video/mp4") ||  matchContentType(type, "video/x-mp4")
    || matchContentType(type, "application/mp4") ||  matchContentType(type, "application/x-mp4");
    
}
bool foobar2000_io::matchContentType_MP4audio( const char * type ) {
    // Gerbera uses audio/x-m4a instead of audio/mp4 ....
    return matchContentType(type, "audio/mp4") || matchContentType(type, "audio/x-mp4") ||
        matchContentType(type, "audio/m4a") || matchContentType(type, "audio/x-m4a");
}
bool foobar2000_io::matchContentType_Ogg( const char * type) {
    return matchContentType(type, "application/ogg") || matchContentType(type, "application/x-ogg") || matchContentType(type, "audio/ogg") || matchContentType(type, "audio/x-ogg");
}
bool foobar2000_io::matchContentType_Opus( const char * type) {
    return matchContentType(type, "audio/opus") || matchContentType(type, "audio/x-opus");
}
bool foobar2000_io::matchContentType_WAV( const char * type ) {
    return matchContentType(type, "audio/vnd.wave" ) || matchContentType(type, "audio/wav") || matchContentType(type, "audio/wave") || matchContentType(type, "audio/x-wav") || matchContentType(type, "audio/x-wave");
}
bool foobar2000_io::matchContentType_FLAC( const char * type) {
    return matchContentType(type, "audio/flac") || matchContentType(type, "audio/x-flac") || matchContentType(type, "application/flac") || matchContentType(type, "application/x-flac");
}
bool foobar2000_io::matchContentType_WavPack( const char * type) {
    return matchContentType( type, "audio/wavpack" ) || matchContentType( type, "audio/x-wavpack");
}
bool foobar2000_io::matchContentType_Musepack( const char * type) {
    return matchContentType(type,"audio/musepack") || matchContentType(type,"audio/x-musepack");
}

pfc::string8 foobar2000_io::getProtocol(const char* fullString) {
	const char* s = strstr(fullString, "://");
	if (s == nullptr) throw exception_io_no_handler_for_path();
	pfc::string8 ret;
	ret.set_string_nc(fullString, s - fullString);
	return ret;
}
const char * foobar2000_io::afterProtocol( const char * fullString ) {
	const char * s = strstr( fullString, "://" );
	if ( s != nullptr ) return s + 3;
	s = strchr(fullString, ':' );
	if ( s != nullptr && s[1] != '\\' && s[1] != 0 ) return s + 1;
	PFC_ASSERT(!"Should not get here");
	return fullString;
}

bool foobar2000_io::testIfHasProtocol( const char * input ) {
    // Take arbitrary untrusted string, return whether looks like foo://
    if ( pfc::char_is_ascii_alpha(input[0])) {
        const char * walk = input+1;
        while(pfc::char_is_ascii_alpha(*walk)) ++ walk;
        if ( walk[0] == ':' && walk[1] == '/' && walk[2] == '/') return true;
    }
    return false;
}

bool foobar2000_io::matchProtocol(const char * fullString, const char * protocolName) {
    const t_size len = strlen(protocolName);
    if (!pfc::stringEqualsI_ascii_ex(fullString, len, protocolName, len)) return false;
    return fullString[len] == ':' && fullString[len+1] == '/' && fullString[len+2] == '/';
}
void foobar2000_io::substituteProtocol(pfc::string_base & out, const char * fullString, const char * protocolName) {
    const char * base = strstr(fullString, "://");
    if (base) {
        out = protocolName; out << base;
    } else {
        PFC_ASSERT(!"Should not get here");
        out = fullString;
    }
}

void filesystem::move_overwrite(const char * src, const char * dst, abort_callback & abort) {
	{
		filesystem_v2::ptr v2;
		if (v2 &= this) {
			v2->move_overwrite(src, dst, abort); return;
		}
	}
	try {
		this->remove(dst, abort);
	} catch (exception_io_not_found const &) {}
	this->move(src, dst, abort);
}

void filesystem::replace_file(const char * src, const char * dst, abort_callback & abort) {
	filesystem_v2::ptr v2;
	if ( v2 &= this ) {
		v2->replace_file(  src, dst, abort ); return;
	}
	move_overwrite( src, dst, abort );
}

void filesystem::make_directory(const char * path, abort_callback & abort, bool * didCreate) {
	filesystem_v2::ptr v2;
	if ( v2 &= this ) {
		v2->make_directory( path, abort, didCreate );
		return;
	}
	bool rv = false;
	try {
		create_directory( path, abort );
		rv = true;
	} catch(exception_io_already_exists const &) {
	}
	if (didCreate != nullptr) * didCreate = rv;
}

bool filesystem::make_directory_check(const char * path, abort_callback & abort) {
	bool rv = false;
	make_directory(path, abort, &rv);
	return rv;
}

bool filesystem::directory_exists(const char * path, abort_callback & abort) {
	filesystem_v2::ptr v2;
	if ( v2 &= this ) {
		return v2->directory_exists( path, abort );
	}
	try {
		directory_callback_dummy cb;
		list_directory(path, cb, abort);
		return true;
	} catch (exception_io const &) { return false; }
}

bool filesystem::exists(const char* path, abort_callback& a) {
	// for rare cases of code test if EITHER FILE OR FOLDER exists at path
	filesystem_v3::ptr v3;
	if (v3 &= this) {
		try {
			v3->get_stats2(path, stats2_fileOrFolder, a);
			return true;
		} catch (exception_io_not_found const &) { return false; }
	}
	filesystem_v2::ptr v2;
	if (v2 &= this) {
		return v2->file_exists(path, a) || v2->directory_exists(path, a);
	}

	try {
		t_filestats stats; bool writable;
		get_stats(path, stats, writable, a);
		return true;
	} catch (exception_io const &) { }
	try {
		directory_callback_dummy cb;
		list_directory(path, cb, a);
		return true;
	} catch (exception_io const &) { }
	return false;
}

bool filesystem::file_exists(const char * path, abort_callback & abort) {
	filesystem_v2::ptr v2;
	if ( v2 &= this ) {
		return v2->file_exists( path, abort );
	}
	try {
		t_filestats stats; bool writable;
		get_stats(path, stats, writable, abort );
		return true;
	} catch(exception_io const &) { return false; }
}

char filesystem::pathSeparator() {
	filesystem_v2::ptr v2;
	if ( v2 &= this ) return v2->pathSeparator();
	return '/';
}

pfc::string8 filesystem::extract_filename_ext(const char* path) {
	pfc::string8 ret; this->extract_filename_ext(path, ret); return ret;
}
pfc::string8 filesystem::get_extension(const char* path) {
	return pfc::string_extension(this->extract_filename_ext(path));
}
pfc::string8 filesystem::g_get_extension(const char* path) {
	auto fs = tryGet(path);
	if (fs.is_valid()) return fs->get_extension(path);
	return pfc::string_extension(path);
}
void filesystem::extract_filename_ext(const char * path, pfc::string_base & outFN) {
	filesystem_v2::ptr v2;
	if ( v2 &= this ) {
		v2->extract_filename_ext( path, outFN );
		return;
	}
	outFN = pfc::filename_ext_v2( path );
}

bool filesystem::get_parent_helper( const char * path, char separator, pfc::string_base & out ) {
	auto proto = path;
	path = afterProtocol(path);

	auto sep_ptr = strrchr( path, separator );
	if ( sep_ptr == path ) return false;
	if ( sep_ptr >= path + 1 && sep_ptr[-1] == separator ) return false;
	
	out.set_string(proto, path - proto);
	out.add_string(path, sep_ptr - path);
	return true;
}

fb2k::stringRef filesystem::parentPath(const char* path) {
	pfc::string8 temp;
	if (!get_parent_path(path, temp)) return nullptr;
	return fb2k::makeString(temp);
}

bool filesystem::get_parent_path(const char * path, pfc::string_base & out) {
	filesystem_v2::ptr v2;
	if ( v2 &= this ) {
		return v2->get_parent_path(path, out);
	}
	return get_parent_helper( path, '/', out );
}

void filesystem::read_whole_file(const char * path, mem_block_container & out, pfc::string_base & outContentType, size_t maxBytes, abort_callback & abort) {
	filesystem_v2::ptr v2;
	if ( v2 &= this ) {
		v2->read_whole_file( path, out, outContentType, maxBytes, abort );
		return;
	}
	read_whole_file_fallback(path, out, outContentType, maxBytes, abort);
}

void filesystem::read_whole_file_fallback(const char * path, mem_block_container & out, pfc::string_base & outContentType, size_t maxBytes, abort_callback & abort) {
	auto f = this->openRead( path, abort, 0 );
	if (!f->get_content_type(outContentType)) outContentType = "";
	auto s64 = f->get_size( abort );
	if ( s64 == filesize_invalid ) {
		// unknown length, perform streamed read
		size_t done = 0, alloc = 0;

		while(alloc < maxBytes ) {
			if ( alloc == 0 ) alloc = 4096;
			else {
				size_t next = alloc * 2;
				if ( next <= alloc ) throw exception_io_data();
				alloc = next;
			}
			if ( alloc > maxBytes ) alloc = maxBytes;
			
			out.set_size( alloc );
			size_t delta = alloc - done;
			size_t deltaGot = f->read( (uint8_t*) out.get_ptr() + done, delta, abort );
			PFC_ASSERT( deltaGot <= delta );
			done += deltaGot;
			if ( deltaGot != delta ) {
				out.set_size( done ); return;
			}
		}
		// maxbytes reached
		PFC_ASSERT( done == maxBytes );
		// corner case check
		if ( f->skip(1, abort) != 0 ) throw exception_io_data();
	} else if ( s64 > maxBytes ) {
		throw exception_io_data();
	} else {
		size_t s = (size_t) s64;
		out.set_size( s );
		if (s > 0) f->read_object( out.get_ptr(), s, abort);
	}
}

bool filesystem::is_transacted() {
#if FB2K_SUPPORT_TRANSACTED_FILESYSTEM
	filesystem_transacted::ptr p;
	return ( p &= this );
#else
	return false;
#endif
}

void filesystem::rewrite_file(const char* path, abort_callback& abort, double opTimeout, const void* payload, size_t bytes) {
	this->rewrite_file(path, abort, opTimeout, [&](file::ptr f) {
		f->write(payload, bytes, abort);
	});
}

void filesystem::rewrite_file(const char * path, abort_callback & abort, double opTimeout, std::function<void(file::ptr) > worker) {
	if ( this->is_transacted() ) {
		auto f = this->openWriteNew( path, abort, opTimeout );
		worker(f);
	} else {
		pfc::string_formatter temp(path); temp << ".new.tmp";
		try {
			{
				auto f = this->openWriteNew( temp, abort, opTimeout );
				worker(f);
				f->flushFileBuffers( abort );
			}

			retryOnSharingViolation(opTimeout, abort, [&] {
				this->replace_file(temp, path, abort);
			});

		} catch(...) {
			try {
				retryOnSharingViolation(opTimeout, abort, [&] { this->remove(temp, fb2k::noAbort); } );
			} catch(...) {}
			throw;
		}
	}
}

void filesystem::rewrite_directory(const char * path, abort_callback & abort, double opTimeout, std::function<void(const char *) > worker) {
	if ( this->is_transacted() ) {
		// so simple
		if ( ! this->make_directory_check( path, abort)  ) {
			retryFileDelete(opTimeout, abort, [&] { this->remove_directory_content(path, abort); });
		}
		worker( path );
	} else {
		// so complex
		pfc::string8 fnNew( path ); fnNew += ".new.tmp";
		pfc::string8 fnOld( path ); fnOld += ".old.tmp";

		if ( !this->make_directory_check( fnNew, abort ) ) {
			// folder.new folder already existed? clear contents
			try {
				retryFileDelete(opTimeout, abort, [&] { this->remove_directory_content(fnNew, abort); });
			} catch(exception_io_not_found const &) {}
		}

		// write to folder.new
		worker( fnNew );

		bool haveOld = false;
		if ( directory_exists( path, abort ) ) {
			// move folder to folder.old
			if (this->directory_exists(fnOld, abort)) {
				try {
					retryFileDelete(opTimeout, abort, [&] { this->remove_object_recur(fnOld, abort); });
				} catch(exception_io_not_found const &) {}
			}
			try {
				retryFileMove(opTimeout, abort, [&] { this->move( path, fnOld, abort ); } ) ;
				haveOld = true;
			} catch(exception_io_not_found const &) {}
		}

		// move folder.new to folder
		retryFileMove( opTimeout, abort, [&] {
			this->move( fnNew, path, abort );
		} );

		if ( haveOld ) {
			// delete folder.old if we made one
			try {
				retryFileDelete( opTimeout, abort, [&] { this->remove_object_recur( fnOld, abort); } );
			} catch (exception_io_not_found const &) {}
		}
	}
}

void filesystem_v2::list_directory(const char * p_path, directory_callback & p_out, abort_callback & p_abort) {
	list_directory_ex(p_path, p_out, listMode::filesAndFolders | listMode::hidden, p_abort);
}

void filesystem_v2::extract_filename_ext(const char * path, pfc::string_base & outFN) {
	outFN = pfc::filename_ext_v2(path, this->pathSeparator() );
}

bool filesystem_v2::get_parent_path(const char * path, pfc::string_base & out) {
	return get_parent_helper(path, pathSeparator(), out);
}

void filesystem_v2::replace_file(const char * src, const char * dst, abort_callback & abort) {
	this->move_overwrite( src, dst, abort );
}

void filesystem_v2::read_whole_file(const char * path, mem_block_container & out, pfc::string_base & outContentType, size_t maxBytes, abort_callback & abort) {
	read_whole_file_fallback( path, out, outContentType, maxBytes, abort );
}

bool filesystem_v2::make_directory_check(const char * path, abort_callback & abort) {
	bool rv = false;
	make_directory(path, abort, &rv);
	return rv;
}

#if FB2K_SUPPORT_TRANSACTED_FILESYSTEM
filesystem_transacted::ptr filesystem_transacted::create( const char * pathFor ) {
	service_enum_t<filesystem_transacted_entry> e;
	filesystem_transacted_entry::ptr p;
	while(e.next(p)) {
		if ( p->is_our_path( pathFor ) ) {
			auto ret = p->create(pathFor);
			if (ret.is_valid()) return ret;
		}
	}
	return nullptr;
}
#endif

bool filesystem::commit_if_transacted(abort_callback &abort) {
	(void)abort;
	bool rv = false;
#if FB2K_SUPPORT_TRANSACTED_FILESYSTEM
	filesystem_transacted::ptr t;
	if ( t &= this ) {
		t->commit( abort ); rv = true;
	}
#endif
	return rv;
}

t_filestats filesystem::get_stats(const char * path, abort_callback & abort) {
	t_filestats s; bool dummy;
	this->get_stats(path, s, dummy, abort);
	return s;
}

bool file_dynamicinfo_v2::get_dynamic_info(class file_info & p_out) {
	t_filesize dummy = 0;
	return this->get_dynamic_info_v2(p_out, dummy);
}

size_t file::lowLevelIO_(const GUID & guid, size_t arg1, void * arg2, size_t arg2size, abort_callback & abort) {
	{
		file_v2::ptr f;
		if (f &= this) return f->lowLevelIO(guid, arg1, arg2, arg2size, abort);
	}
	{
		file_lowLevelIO::ptr f;
		if (f &= this) return f->lowLevelIO(guid, arg1, arg2, arg2size, abort);
	}
	return 0;
}

bool file::flushFileBuffers(abort_callback & abort) {
	return this->lowLevelIO_(file_lowLevelIO::guid_flushFileBuffers, 0, nullptr, 0, abort) != 0;
}

bool file::getFileTimes(filetimes_t & out, abort_callback & a) {
	return this->lowLevelIO_(file_lowLevelIO::guid_getFileTimes, 0, &out, sizeof(out), a) != 0;
}

bool file::setFileTimes(filetimes_t const & in, abort_callback & a) {
	return this->lowLevelIO_(file_lowLevelIO::guid_setFileTimes, 0, (void*)&in, sizeof(in), a) != 0;
}

bool file::g_copy_creation_time(service_ptr_t<file> from, service_ptr_t<file> to, abort_callback& a) {
	bool rv = false;
	auto ft = from->get_time_created(a);
	if (ft != filetimestamp_invalid) {
		filetimes_t ft2;
		ft2.creation = ft;
		rv = to->setFileTimes(ft2, a);
	}
	return rv;
}
bool file::g_copy_timestamps(file::ptr from, file::ptr to, abort_callback& a) {
	{
		filetimes_t filetimes = {};
		if (from->getFileTimes(filetimes, a)) {
			return to->setFileTimes(filetimes, a);
		}
	}
	filetimes_t filetimes = {};
	auto stats = from->get_stats2_(stats2_timestamp | stats2_timestampCreate, a);
	if (stats.m_timestamp != filetimestamp_invalid || stats.m_timestampCreate != filetimestamp_invalid) {
		filetimes.lastWrite = stats.m_timestamp; filetimes.creation = stats.m_timestampCreate;
		return to->setFileTimes(filetimes, a);
	}
	return false;
}

service_ptr file::get_metadata_(abort_callback& a) {
	service_ptr ret;
	file_v2::ptr getter;
	if (getter &= this) ret = getter->get_metadata(a);
	return ret;
}

drivespace_t filesystem_v3::getDriveSpace(const char*, abort_callback&) {
	throw pfc::exception_not_implemented();
}

t_filestats2 filesystem::get_stats2_(const char* p_path, uint32_t s2flags, abort_callback& p_abort) {
	filesystem_v3::ptr api3;
	t_filestats2 ret;
	if (api3 &= this) {
		ret = api3->get_stats2(p_path, s2flags, p_abort);
	} else {
		if (this->directory_exists(p_path, p_abort)) {
			ret.set_folder(true);
		} else if (s2flags & (stats2_size | stats2_timestamp | stats2_canWrite)) {
			t_filestats temp;
			bool canWrite = false;
			this->get_stats(p_path, temp, canWrite, p_abort);
			ret.m_size = temp.m_size;
			ret.m_timestamp = temp.m_timestamp;
			ret.set_file();
			ret.set_readonly(!canWrite);
		}
		if ( s2flags & stats2_remote ) ret.set_remote(this->is_remote(p_path));
	}
	return ret;

}

t_filestats2 filesystem::g_get_stats2(const char* p_path, uint32_t s2flags, abort_callback& p_abort) {
	return get(p_path)->get_stats2_(p_path, s2flags, p_abort);
}

void filesystem_v3::get_stats(const char* p_path, t_filestats& p_stats, bool& p_is_writeable, abort_callback& p_abort) {
	t_filestats2 s2 = this->get_stats2(p_path, stats2_canWrite | stats2_size | stats2_timestamp, p_abort);
	p_stats = s2.to_legacy();
	p_is_writeable = s2.can_write();
}

t_filetimestamp file_v2::get_timestamp(abort_callback& p_abort) {
	return this->get_stats2(stats2_timestamp, p_abort).m_timestamp;
}
bool file_v2::is_remote() {
	return this->get_stats2(stats2_remote, fb2k::noAbort).is_remote();
}

t_filesize file_v2::get_size(abort_callback& p_abort) {
	return this->get_stats2(stats2_size, p_abort).m_size;
}

pfc::string8 file::get_content_type() {
	pfc::string8 ret;
	if (!this->get_content_type(ret)) ret.clear();
	return ret;

}
t_filestats2 file::get_stats2_(uint32_t f, abort_callback& a) {
	t_filestats2 ret;

	file_v2::ptr v2;
	if (v2 &= this) {
		ret = v2->get_stats2(f, a);
		PFC_ASSERT(ret.is_file());
	} else {
		if (f & stats2_size) ret.m_size = this->get_size(a);
		if (f & stats2_timestamp) ret.m_timestamp = this->get_timestamp(a);
		ret.set_file();
		ret.set_remote(this->is_remote());
		// we do not know if it's readonly or not, can_write() tells us if the file was open for writing, not if it can possibly be opened for writing
	}
	return ret;
}

pfc::string8 t_filestats2::format_attribs(uint32_t attr, const char * delim) {
	pfc::string8 ret;
	if (attr != 0) {
		const char* arr[5] = {};
		size_t w = 0;
		ret.prealloc(64);
		if (attr & attr_readonly) {
			arr[w++] = "read-only";
		}
		if (attr & attr_folder) {
			arr[w++] = "folder";
		}
		if (attr & attr_hidden) {
			arr[w++] = "hidden";
		}
		if (attr & attr_system) {
			arr[w++] = "system";
		}
		if (attr & attr_remote) {
			arr[w++] = "remote";
		}
		PFC_ASSERT(w <= PFC_TABSIZE(arr));
		for (size_t f = 0; f < w; ++f) {
			if (f > 0) ret += delim;
			ret += arr[f];
		}
	}
	return ret;
}

t_filetimestamp file::get_time_created(abort_callback& a) {
	t_filetimestamp ret;
	ret = get_stats2_(stats2_timestampCreate, a).m_timestampCreate;
	if (ret != filetimestamp_invalid) return ret;

	filetimes_t ft;
	if (this->getFileTimes(ft, a)) return ft.creation;
	return filetimestamp_invalid;
}

bool filesystem::get_display_name_short_(const char* path, pfc::string_base& out) {

	try {
		filesystem_v3::ptr v3;
		if (v3 &= this) return v3->get_display_name_short(path, out);
    } catch(...) {return false;} // handle nonsense path etc

	pfc::string8 temp;
	extract_filename_ext(path, temp);
	if (temp.length() == 0) return false;
	out = temp;
	return true;
}

bool filesystem_v3::get_display_name_short(const char* path, pfc::string_base& out) {
	pfc::string8 temp;
	extract_filename_ext(path, temp);
	if (temp.length() == 0) return false;
	out = temp;
	return true;
}

fb2k::memBlockRef filesystem::readWholeFile(const char* path, size_t maxBytes, abort_callback& abort) {
	mem_block_container_impl temp;
	pfc::string8 contentType;
	this->read_whole_file(path, temp, contentType, maxBytes, abort);
	return fb2k::memBlock::blockWithData(std::move(temp.m_data));
}

fb2k::memBlockRef filesystem::g_readWholeFile(const char * path, size_t sizeSanity, abort_callback & aborter) {
    return get(path)->readWholeFile(path, sizeSanity, aborter);
}

namespace {
	class directory_callback_v3_to_legacy : public directory_callback_v3 {
	public:
		directory_callback* m_chain = nullptr;

		bool on_entry(filesystem* owner, const char* URL, t_filestats2 const& stats, abort_callback& abort) override {
			return m_chain->on_entry(owner, abort, URL, stats.is_folder(), stats.as_legacy());
		}
	};
}

void filesystem_v3::list_directory_ex(const char* p_path, directory_callback& p_out, unsigned listMode, abort_callback& p_abort) {
	directory_callback_v3_to_legacy wrap;
	wrap.m_chain = &p_out;
	this->list_directory_v3(p_path, wrap, listMode, p_abort);
}

bool filesystem_v3::directory_exists(const char* path, abort_callback& abort) {
	try {
		return get_stats2(path, stats2_fileOrFolder, abort).is_folder();
	} catch (exception_io_not_found const &) { return false; }
}

bool filesystem_v3::file_exists(const char* path, abort_callback& abort) {
	try {
		return get_stats2(path, stats2_fileOrFolder, abort).is_file();
	} catch (exception_io_not_found const &) { return false; }
}


namespace {
	class directory_callback_v3_to_lambda : public directory_callback_v3 {
	public:
		filesystem::list_callback_t f;
		bool on_entry(filesystem*, const char* URL, t_filestats2 const& stats, abort_callback&) override {
			f(URL, stats);
			return true;
		}
	};
	class directory_callback_to_lambda : public directory_callback {
	public:
		filesystem::list_callback_t f;

		bool m_enforceListMode = false;
		unsigned m_listMode = 0;

		bool on_entry(filesystem* p_owner, abort_callback& p_abort, const char* p_url, bool p_is_subdirectory, const t_filestats& p_stats) override {
			(void)p_owner; p_abort.check();
			if (m_enforceListMode) {
				if (p_is_subdirectory) {
					if ( (m_listMode & listMode::folders) == 0 ) return true;
				} else {
					if ((m_listMode & listMode::files) == 0) return true;
				}
			}

			t_filestats2 stats;
			stats.set_folder(p_is_subdirectory);
			stats.m_size = p_stats.m_size;
			stats.m_timestamp = p_stats.m_timestamp;
			f(p_url, stats);
			return true;
		}
	};
}

void filesystem::list_directory_(const char* path, list_callback_t f, unsigned listMode, abort_callback& a) {
	
	{
		filesystem_v3::ptr v3;
		if (v3 &= this) {
			directory_callback_v3_to_lambda cb;
			cb.f = f;
			v3->list_directory_v3(path, cb, listMode, a);
			return;
		}
	}

	{
		filesystem_v2::ptr v2;
		if (v2 &= this) {
			directory_callback_to_lambda cb;
			cb.f = f;
			v2->list_directory_ex(path, cb, listMode, a);
			return;
		}
	}

	directory_callback_to_lambda cb;
	cb.m_listMode = listMode;
	cb.m_enforceListMode = true;
	cb.f = f;
	this->list_directory(path, cb, a);
}


fsItemBase::ptr filesystem_v3::findItem(const char* path, abort_callback& p_abort) {
#if PFC_DEBUG
	try {
#endif
		pfc::string8 canonical;
		if (get_canonical_path(path, canonical)) {
			auto stats = this->get_stats2(path, stats2_all, p_abort);
			if ( stats.is_folder() ) {
				return makeItemFolderStd(canonical, stats);
			} else {
				return makeItemFileStd(canonical, stats );
			}
		}
		throw exception_io_not_found();
#if PFC_DEBUG
	} catch (std::exception const& e) {
		try {
			FB2K_console_formatter() << "filesystem::findItem error: " << e;
			FB2K_console_formatter() << "problem path: " << path;
		} catch (...) {}
		throw;
	}
#endif
}
fsItemFile::ptr filesystem_v3::findItemFile(const char* path, abort_callback& p_abort) {
#if PFC_DEBUG
	try {
#endif
		pfc::string8 canonical;
		if (get_canonical_path(path, canonical)) {
			auto stats = this->get_stats2( canonical, stats2_all, p_abort);
			if ( stats.is_file() ) {
				return makeItemFileStd(canonical, stats );
			}
		}
		throw exception_io_not_found();
#if PFC_DEBUG
	} catch (std::exception const& e) {
		try {
			FB2K_console_formatter() << "filesystem::findItemFile error: " << e;
			FB2K_console_formatter() << "problem path: " << path;
		} catch (...) {}
		throw;
	}
#endif
}
fsItemFolder::ptr filesystem_v3::findItemFolder(const char* path, abort_callback& p_abort) {
#if PFC_DEBUG
	try {
#endif
		pfc::string8 canonical;
		if (get_canonical_path(path, canonical)) {
			auto stats = this->get_stats2( canonical, stats2_all, p_abort);
			if ( stats.is_folder() ) {
				return makeItemFolderStd(canonical, stats );
			}
		}
		throw exception_io_not_found();
#if PFC_DEBUG
	} catch (std::exception const& e) {
		try {
			FB2K_console_formatter() << "filesystem::findItemFolder error: " << e;
			FB2K_console_formatter() << "problem path: " << path;
		} catch (...) {}
		throw;
	}
#endif
}

fsItemFolder::ptr filesystem_v3::findParentFolder(const char* path, abort_callback& p_abort) {
	auto parent = parentPath(path);
	if (parent == nullptr) {
#if PFC_DEBUG
		FB2K_console_formatter() << "filesystem::findParentFolder error: cannot walk up from root path";
		FB2K_console_formatter() << "problem path: " << path;
#endif
		throw exception_io_not_found();
	}
	return findItemFolder(parent->c_str(), p_abort);
}

fb2k::stringRef filesystem_v3::fileNameSanity(const char* fn) {
	auto ret = fb2k::makeString(pfc::io::path::validateFileName(fn).c_str());
	if (ret->length() == 0) ret = fb2k::makeString("_");
	return ret;
}

static void readStatsMultiStd(fb2k::arrayRef items, uint32_t s2flags, t_filestats2* outStats, abort_callback& abort) {
	const size_t count = items->size();
	for (size_t w = 0; w < count; ++w) {
		abort.check();
		auto& out = outStats[w];
		try {
			fsItemPtr f; f ^= items->itemAt(w);
			out = f->getStats2(s2flags, abort);
		} catch (exception_aborted const &) {
			throw;
		} catch (exception_io const &) {
			out = filestats2_invalid;
		}
	}
}

void filesystem_v3::readStatsMulti(fb2k::arrayRef items, uint32_t s2flags, t_filestats2* outStats, abort_callback& abort) {
	readStatsMultiStd(items, s2flags, outStats, abort);
}

pfc::string8 t_filestats::describe() const {
	pfc::string8 ret;
	ret << "size: ";
	if (m_size != filesize_invalid) ret << m_size;
	else ret << "N/A";
	ret << "\n";
	ret << "last-modified: ";
	if (m_timestamp != filetimestamp_invalid) ret << m_timestamp;
	else ret << "N/A";
	ret << "\n";
	return ret;
}

pfc::string8 t_filestats2::describe() const {
	pfc::string8 ret;
	ret << "size: ";
	if (m_size != filesize_invalid) ret << m_size;
	else ret << "N/A";
	ret << "\n";
	ret << "last-modified: ";
	if (m_timestamp != filetimestamp_invalid) ret << m_timestamp;
	else ret << "N/A";
	ret << "\n";
	ret << "created: ";
	if (m_timestampCreate != filetimestamp_invalid) ret << m_timestampCreate;
	else ret << "N/A";
	ret << "\n";
	ret << "attribs: " << format_attribs() << "\n";
	ret << "attribs valid: " << format_attribs(m_attribsValid) << "\n";
	return ret;
}


#ifndef _WIN32
t_filestats foobar2000_io::nixMakeFileStats(const struct stat & st) {
    t_filestats out = filestats_invalid;
    out.m_size = st.st_size;
#ifdef __APPLE__
    out.m_timestamp = pfc::fileTimeUtoW(st.st_mtimespec);
#else // Linux
	out.m_timestamp = pfc::fileTimeUtoW(st.st_mtim);
#endif
    return out;
}

bool foobar2000_io::nixQueryReadonly( const struct stat & st ) {
    return (st.st_mode & 0222) == 0;
}

bool foobar2000_io::nixQueryDirectory( const struct stat & st ) {
    return (st.st_mode & S_IFDIR) != 0;
}

t_filestats2 foobar2000_io::nixMakeFileStats2(const struct stat &st) {
    t_filestats2 ret = t_filestats2::from_legacy( nixMakeFileStats( st ) );
#ifdef __APPLE__
	ret.m_timestampCreate = pfc::fileTimeUtoW(st.st_birthtimespec);
#else // Linux
	ret.m_timestampCreate = pfc::fileTimeUtoW(st.st_ctim);
#endif
    ret.set_readonly(nixQueryReadonly(st));
    if ( st.st_mode & S_IFDIR ) ret.set_folder();
    else if (st.st_mode & S_IFREG ) ret.set_file();
	ret.set_remote(false);
    return ret;
}

bool foobar2000_io::compactHomeDir(pfc::string_base & str) {
    const char * prefix = "file://";
    size_t base = 0;
    if (pfc::strcmp_partial( str, prefix ) == 0) base = strlen(prefix);
    const char * homedir = getenv("HOME");
    if (homedir == NULL) return false;
    const char * strBase = str.get_ptr() + base;
    if (pfc::strcmp_partial(strBase, homedir) == 0) {
        const char * strPast = strBase + strlen(homedir);
        if (*strPast == 0 || *strPast == '/') {
            pfc::string8 temp ( strPast );
            str.truncate( base );
            str += "~";
            str += temp;
            return true;
        }
    }
    
    return false;
}

bool foobar2000_io::expandHomeDir(pfc::string_base & str) {
    const char * prefix = "file://";
    size_t base = 0;
    if (pfc::strcmp_partial( str, prefix ) == 0) base = strlen(prefix);
    const char * strBase = str.get_ptr() + base;
    if (strBase[0] == '~') {
        if (strBase[1] == 0 || strBase[1] == '/') {
            const char * homedir = getenv("HOME");
            if (homedir == NULL) return false;
            pfc::string8 temp( strBase + 1 );
            str.truncate( base );
            str += homedir;
            str += temp;
            return true;
        }
    }
    
    return false;
}

#endif // _WIN32

bool filesystem::g_get_display_name_short( const char * path, pfc::string_base & out ) {
    auto i = tryGet( path );
    if ( i.is_valid() ) {
        if (i->get_display_name_short_(path, out)) return true;
    }
    out = path;
    return false;
}


bool filesystem::g_compare_paths(const char* p1, const char* p2, int& result) {
	if (strcmp(p1, p2) == 0) {
		result = 0; return true;
	}

	{
		auto s1 = strstr(p1, "://");
		auto s2 = strstr(p2, "://");
		if (s1 == nullptr || s2 == nullptr) {
			PFC_ASSERT(!"Invalid arguments");
			return false;
		}
		size_t prefix = s1 - p1;
		if (prefix != (size_t)(s2 - p2)) return false; // protocol mismatch
		if (memcmp(p1, p2, prefix) != 0) return false; // protocol mismatch
	}

	filesystem::ptr fs;
	if (!g_get_interface(fs, p1)) {
		PFC_ASSERT(!"Invalid arguments");
		return false;
	}
	pfc::string8 temp1(p1), temp2(p2);
	auto delim = fs->pathSeparator();
	temp1.end_with(delim); temp2.end_with(delim);
	if (strcmp(temp1, temp2) == 0) { result = 0; return true; }

	//result 1 if p2 is a subpath of p1, -1 if p1 is a subpath of p2
	if (pfc::string_has_prefix(temp1, temp2)) {
		// temp1 starts with temp2
		// p1 a subfolder of p2
		result = -1;
		return true;
	} else if (pfc::string_has_prefix(temp2, temp1)) {
		// temp2 starts with temp1
		// p2 a subfolder of p1
		result = 1;
		return true;
	} else {
		return false;
	}
}

size_t file::receive(void* ptr, size_t bytes, abort_callback& a) {
	stream_receive::ptr obj;
	if (obj &= this) return obj->receive(ptr, bytes, a);
	else return this->read(ptr, bytes, a);
}

void filesystem::g_readStatsMulti(fb2k::arrayRef items, uint32_t s2flags, t_filestats2* outStats, abort_callback& abort) {
	if (items->size() == 0) return;
	fsItemPtr aFile; aFile ^= items->itemAt(0);
	filesystem_v3::ptr fs;
	if (fs &= aFile->getFS()) {
		fs->readStatsMulti(items, s2flags, outStats, abort);
	} else {
		readStatsMultiStd(items, s2flags, outStats, abort);
	}
}

void file::set_stats(t_filestats2 const& stats, abort_callback& a) {
	if (stats.haveTimestamp() || stats.haveTimestampCreate()) {
		filetimes_t ft;
		ft.creation = stats.m_timestampCreate;
		ft.lastWrite = stats.m_timestamp;
		this->setFileTimes(ft, a);
	}
}

fb2k::stringRef filesystem::fileNameSanity_(const char* fn) {
    filesystem_v3::ptr v3;
    if (v3 &= this) return v3->fileNameSanity(fn);
    throw pfc::exception_not_implemented();
}

drivespace_t filesystem::getDriveSpace_(const char* pathAt, abort_callback& abort) {
    filesystem_v3::ptr v3;
    if (v3 &= this) return v3->getDriveSpace(pathAt, abort);
    throw pfc::exception_not_implemented();
}

size_t stream_receive::read_using_receive(void* ptr_, size_t bytes, abort_callback& a) {
    size_t walk = 0;
    auto ptr = reinterpret_cast<uint8_t*>(ptr_);
    while(walk < bytes) {
        size_t want = bytes-walk;
        size_t delta = this->receive(ptr+walk, want, a);
        PFC_ASSERT( delta <= want );
        if ( delta == 0 ) break;
        walk += delta;
    }
    return walk;
}