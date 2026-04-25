#pragma once

#include <memory>
#include <set>
#include <string>

#include "pbd/signals.h"

namespace ARDOUR {

class Session;
class SuperColliderTrack;
class SystemExec;

class SuperColliderSessionRuntime : public PBD::ScopedConnectionList
{
public:
	explicit SuperColliderSessionRuntime (Session&);
	~SuperColliderSessionRuntime ();

	bool runtime_available () const;
	std::string runtime_path () const;
	bool running () const;
	bool track_active (SuperColliderTrack const&) const;
	std::string const& last_error () const { return _last_error; }

	bool activate_track (SuperColliderTrack const&);
	void deactivate_track (SuperColliderTrack const&);
	void stop ();
	void sync_transport ();

private:
	static std::string string_literal (std::string const&);
	static std::string track_key (SuperColliderTrack const&);

	bool ensure_started ();
	bool send_code (std::string const&);
	std::string bootstrap_code () const;
	std::string track_update_code (SuperColliderTrack const&) const;
	std::string track_stop_code (SuperColliderTrack const&) const;
	void runtime_output (std::string, size_t);
	void runtime_terminated ();

	Session& _session;
	std::unique_ptr<SystemExec> _runtime;
	PBD::ScopedConnectionList _runtime_connections;
	std::set<std::string> _active_tracks;
	std::string _last_error;
};

} // namespace ARDOUR
