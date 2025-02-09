#pragma once
#include "callback_merit.h"

// IMPLEMENTATION NOTE: all callback methods declared here could not be declared noexcept for historical reasons, but it's strongly recommended that your overrides of these methods are noexcept.

/*!
Class receiving notifications about playback events. Note that all methods are called only from app's main thread.
Use play_callback_manager to register your dynamically created instances. Statically registered version is available too - see play_callback_static.
*/
class NOVTABLE play_callback {
public:
	//! Playback process is being initialized. on_playback_new_track() should be called soon after this when first file is successfully opened for decoding.
	virtual void on_playback_starting(play_control::t_track_command p_command,bool p_paused) = 0;
	//! Playback advanced to new track.
	virtual void on_playback_new_track(metadb_handle_ptr p_track) = 0;
	//! Playback stopped.
	virtual void on_playback_stop(play_control::t_stop_reason p_reason) = 0;
	//! User has seeked to specific time.
	virtual void on_playback_seek(double p_time) = 0;
	//! Called on pause/unpause.
	virtual void on_playback_pause(bool p_state) = 0;
	//! Called when currently played file gets edited.
	virtual void on_playback_edited(metadb_handle_ptr p_track) = 0;
	//! Dynamic info (VBR bitrate etc) change.
	virtual void on_playback_dynamic_info(const file_info & p_info) = 0;
	//! Per-track dynamic info (stream track titles etc) change. Happens less often than on_playback_dynamic_info().
	virtual void on_playback_dynamic_info_track(const file_info & p_info) = 0;
	//! Called every second, for time display
	virtual void on_playback_time(double p_time) = 0;
	//! User changed volume settings. Possibly called when not playing.
	//! @param p_new_val new volume level in dB; 0 for full volume.
	virtual void on_volume_change(float p_new_val) = 0;

	enum {
		flag_on_playback_starting			= 1 << 0,
		flag_on_playback_new_track			= 1 << 1, 
		flag_on_playback_stop				= 1 << 2,
		flag_on_playback_seek				= 1 << 3,
		flag_on_playback_pause				= 1 << 4,
		flag_on_playback_edited				= 1 << 5,
		flag_on_playback_dynamic_info		= 1 << 6,
		flag_on_playback_dynamic_info_track	= 1 << 7,
		flag_on_playback_time				= 1 << 8,
		flag_on_volume_change				= 1 << 9,

		flag_on_playback_all = flag_on_playback_starting | flag_on_playback_new_track | 
			flag_on_playback_stop | flag_on_playback_seek | 
			flag_on_playback_pause | flag_on_playback_edited |
			flag_on_playback_dynamic_info | flag_on_playback_dynamic_info_track | flag_on_playback_time,
	};
protected:
	play_callback() {}
	~play_callback() {}
};

//! Standard API (always present); manages registrations of dynamic play_callbacks. \n
//! Usage: use play_callback_manager::get() to obtain on instance. \n
//! Do not reimplement.
class NOVTABLE play_callback_manager : public service_base {
	FB2K_MAKE_SERVICE_COREAPI(play_callback_manager);
public:
	//! Registers a play_callback object.
	//! @param p_callback Interface to register.
	//! @param p_flags Indicates which notifications are requested.
	//! @param p_forward_status_on_register Set to true to have the callback immediately receive current playback status as notifications if playback is active (eg. to receive info about playback process that started before our callback was registered).
	virtual void register_callback(play_callback * p_callback,unsigned p_flags,bool p_forward_status_on_register) = 0;
	//! Unregisters a play_callback object.
	//! @p_callback Previously registered interface to unregister.
	virtual void unregister_callback(play_callback * p_callback) = 0;
};

//! \since 2.0
class NOVTABLE play_callback_manager_v2 : public play_callback_manager {
	FB2K_MAKE_SERVICE_COREAPI_EXTENSION(play_callback_manager_v2, play_callback_manager);
public:
	virtual void set_callback_merit(play_callback*, fb2k::callback_merit_t) = 0;
};

//! Implementation helper.
class play_callback_impl_base : public play_callback {
public:
	play_callback_impl_base(unsigned p_flags = 0xFFFFFFFF) {
        if (p_flags != 0) play_callback_manager::get()->register_callback(this,p_flags,false);
	}
	~play_callback_impl_base() {
		play_callback_manager::get()->unregister_callback(this);
	}
	void play_callback_reregister(unsigned flags, bool refresh = false) {
		auto api = play_callback_manager::get();
		api->unregister_callback(this);
		if (flags != 0) api->register_callback(this,flags,refresh);
	}
	void on_playback_starting(play_control::t_track_command p_command, bool p_paused) override { (void)p_command; (void)p_paused; }
	void on_playback_new_track(metadb_handle_ptr p_track) override { (void)p_track; }
	void on_playback_stop(play_control::t_stop_reason p_reason) override { (void)p_reason; }
	void on_playback_seek(double p_time) override { (void)p_time; }
	void on_playback_pause(bool p_state) override { (void)p_state; }
	void on_playback_edited(metadb_handle_ptr p_track) override { (void)p_track; }
	void on_playback_dynamic_info(const file_info& p_info) override { (void)p_info; }
	void on_playback_dynamic_info_track(const file_info& p_info) override { (void)p_info; }
	void on_playback_time(double p_time) override { (void)p_time; }
	void on_volume_change(float p_new_val) override { (void)p_new_val; }

	PFC_CLASS_NOT_COPYABLE_EX(play_callback_impl_base)
};

//! Static (autoregistered) version of play_callback. Use play_callback_static_factory_t to register.
class play_callback_static : public service_base, public play_callback {
	FB2K_MAKE_SERVICE_INTERFACE_ENTRYPOINT(play_callback_static);
public:
	//! Controls which methods your callback wants called; returned value should not change in run time, you should expect it to be queried only once (on startup). See play_callback::flag_* constants.
	virtual unsigned get_flags() = 0;
};

template<typename T>
class play_callback_static_factory_t : public service_factory_single_t<T> {};


//! Gets notified about tracks being played. Notification occurs when at least 60s of the track has been played, or the track has reached its end after at least 1/3 of it has been played through.
//! Use playback_statistics_collector_factory_t to register.
class NOVTABLE playback_statistics_collector : public service_base {
	FB2K_MAKE_SERVICE_INTERFACE_ENTRYPOINT(playback_statistics_collector);
public:
	virtual void on_item_played(metadb_handle_ptr p_item) = 0;
};

template<typename T>
class playback_statistics_collector_factory_t : public service_factory_single_t<T> {};




//! Helper providing a simplified interface for receiving playback events, in case your code does not care about the kind of playback event that has occurred; useful typically for GUI/rendering code that just refreshes some control whenever a playback state change occurs.
class playback_event_notify : private play_callback_impl_base {
public:
	playback_event_notify(playback_control::t_display_level level = playback_control::display_level_all) : play_callback_impl_base(GrabCBFlags(level)) {}

	static t_uint32 GrabCBFlags(playback_control::t_display_level level) {
		t_uint32 flags = flag_on_playback_starting | flag_on_playback_new_track | flag_on_playback_stop | flag_on_playback_pause | flag_on_playback_edited | flag_on_volume_change;
		if (level >= playback_control::display_level_titles) flags |= flag_on_playback_dynamic_info_track;
		if (level >= playback_control::display_level_all) flags |= flag_on_playback_seek | flag_on_playback_dynamic_info | flag_on_playback_time;
		return flags;
	}
protected:
	virtual void on_playback_event() {}
private:
	void on_playback_starting(play_control::t_track_command,bool) override {on_playback_event();}
	void on_playback_new_track(metadb_handle_ptr) override {on_playback_event();}
	void on_playback_stop(play_control::t_stop_reason) override {on_playback_event();}
	void on_playback_seek(double) override {on_playback_event();}
	void on_playback_pause(bool) override {on_playback_event();}
	void on_playback_edited(metadb_handle_ptr) override {on_playback_event();}
	void on_playback_dynamic_info(const file_info &) override {on_playback_event();}
	void on_playback_dynamic_info_track(const file_info &) override {on_playback_event();}
	void on_playback_time(double) override {on_playback_event();}
	void on_volume_change(float) override {on_playback_event();}
};

class playback_volume_notify : private play_callback_impl_base {
public:	
	playback_volume_notify() : play_callback_impl_base(flag_on_volume_change) {}
	// override me
	void on_volume_change(float p_new_val) override { (void)p_new_val; }
};
