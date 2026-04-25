#include "ardour/supercollider_session.h"

#include <cstdlib>
#include <cstring>
#include <functional>

#include <glib.h>

#include "ardour/session.h"
#include "ardour/supercollider_track.h"
#include "ardour/system_exec.h"

#include "pbd/compose.h"
#include "pbd/error.h"

#include "pbd/i18n.h"

using namespace ARDOUR;

SuperColliderSessionRuntime::SuperColliderSessionRuntime (Session& session)
	: _session (session)
{
	_session.TransportStateChange.connect_same_thread (*this, std::bind (&SuperColliderSessionRuntime::sync_transport, this));
}

SuperColliderSessionRuntime::~SuperColliderSessionRuntime ()
{
	drop_connections ();
	stop ();
}

bool
SuperColliderSessionRuntime::runtime_available () const
{
	return !runtime_path ().empty ();
}

std::string
SuperColliderSessionRuntime::runtime_path () const
{
	gchar* path = g_find_program_in_path ("sclang");
	if (path) {
		std::string runtime_path (path);
		g_free (path);
		return runtime_path;
	}

#ifdef __APPLE__
	char const* candidates[] = {
		"/Applications/SuperCollider.app/Contents/MacOS/sclang",
		"/Applications/SuperCollider.app/Contents/Resources/sclang",
		0
	};

	for (char const** candidate = candidates; *candidate; ++candidate) {
		if (g_file_test (*candidate, G_FILE_TEST_IS_EXECUTABLE)) {
			return *candidate;
		}
	}
#endif

	return "";
}

bool
SuperColliderSessionRuntime::running () const
{
	return _runtime && const_cast<SystemExec*> (_runtime.get())->is_running ();
}

bool
SuperColliderSessionRuntime::track_active (SuperColliderTrack const& track) const
{
	return _active_tracks.find (track_key (track)) != _active_tracks.end ();
}

bool
SuperColliderSessionRuntime::activate_track (SuperColliderTrack const& track)
{
	if (!ensure_started ()) {
		return false;
	}

	if (!send_code (track_update_code (track))) {
		_last_error = _("could not send code to sclang");
		return false;
	}

	_active_tracks.insert (track_key (track));
	_last_error.clear ();
	return true;
}

void
SuperColliderSessionRuntime::deactivate_track (SuperColliderTrack const& track)
{
	if (running ()) {
		send_code (track_stop_code (track));
	}

	_active_tracks.erase (track_key (track));

	if (_active_tracks.empty ()) {
		stop ();
	}
}

void
SuperColliderSessionRuntime::stop ()
{
	_active_tracks.clear ();
	_runtime_connections.drop_connections ();

	if (_runtime) {
		_runtime->terminate ();
		_runtime.reset ();
	}
}

void
SuperColliderSessionRuntime::sync_transport ()
{
	if (!running ()) {
		return;
	}

	send_code (string_compose (
		"~ardourTransportRolling = %1;\n"
		"~ardourTransportSample = %2;\n",
		_session.transport_state_rolling () ? "true" : "false",
		std::to_string (_session.transport_sample ())
	));
}

std::string
SuperColliderSessionRuntime::string_literal (std::string const& value)
{
	std::string escaped = "\"";

	for (std::string::const_iterator i = value.begin (); i != value.end (); ++i) {
		switch (*i) {
		case '\\':
			escaped += "\\\\";
			break;
		case '"':
			escaped += "\\\"";
			break;
		case '\n':
			escaped += "\\n";
			break;
		case '\r':
			escaped += "\\r";
			break;
		case '\t':
			escaped += "\\t";
			break;
		default:
			escaped += *i;
			break;
		}
	}

	escaped += "\"";
	return escaped;
}

std::string
SuperColliderSessionRuntime::track_key (SuperColliderTrack const& track)
{
	return track.id ().to_s ();
}

bool
SuperColliderSessionRuntime::ensure_started ()
{
	if (running ()) {
		return true;
	}

	std::string const path = runtime_path ();
	if (path.empty ()) {
		_last_error = _("sclang not found");
		return false;
	}

	char** argp = static_cast<char**> (calloc (2, sizeof (char*)));
	argp[0] = strdup (path.c_str ());
	argp[1] = 0;

	_runtime.reset (new SystemExec (argp[0], argp, true));
	_runtime->ReadStdout.connect_same_thread (
		_runtime_connections, std::bind (&SuperColliderSessionRuntime::runtime_output, this, std::placeholders::_1, std::placeholders::_2)
	);
	_runtime->Terminated.connect_same_thread (
		_runtime_connections, std::bind (&SuperColliderSessionRuntime::runtime_terminated, this)
	);

	if (_runtime->start (SystemExec::MergeWithStdin)) {
		_last_error = _("could not launch sclang");
		_runtime_connections.drop_connections ();
		_runtime.reset ();
		return false;
	}

	if (!send_code (bootstrap_code ())) {
		_last_error = _("could not initialize sclang");
		stop ();
		return false;
	}

	sync_transport ();
	_last_error.clear ();
	return true;
}

bool
SuperColliderSessionRuntime::send_code (std::string const& code)
{
	if (!running ()) {
		return false;
	}

	return _runtime->write_to_stdin (code + "\n") > 0;
}

std::string
SuperColliderSessionRuntime::bootstrap_code () const
{
	return
		"(\n"
		"~ardourTracks = ~ardourTracks ? IdentityDictionary.new;\n"
		"Server.default = Server.default ? Server.local;\n"
		"s = Server.default;\n"
		"s.waitForBoot({\n"
		"    \"[Ardour] SuperCollider session ready\".postln;\n"
		"});\n"
		"s.boot;\n"
		")\n";
}

std::string
SuperColliderSessionRuntime::track_update_code (SuperColliderTrack const& track) const
{
	return string_compose (
		"(\n"
		"var trackId = %1;\n"
		"var trackName = %2;\n"
		"var synthdefName = %3;\n"
		"~ardourTracks = ~ardourTracks ? IdentityDictionary.new;\n"
		"s = Server.default ? Server.local;\n"
		"s.waitForBoot({\n"
		"    var oldState = ~ardourTracks[trackId];\n"
		"    var trackGroup;\n"
		"    if (oldState.notNil) {\n"
		"        if (oldState[\\group].notNil) {\n"
		"            oldState[\\group].freeAll;\n"
		"            oldState[\\group].free;\n"
		"        };\n"
		"    };\n"
		"    trackGroup = Group.tail(s);\n"
		"    ~ardourTrackId = trackId;\n"
		"    ~ardourTrackName = trackName;\n"
		"    ~ardourSynthDef = synthdefName;\n"
		"    ~ardourTrackGroup = trackGroup;\n"
		"    ~ardourTracks[trackId] = (name: trackName, synthdef: synthdefName, group: trackGroup);\n"
		"%4\n"
		"});\n"
		")\n",
		string_literal (track_key (track)),
		string_literal (track.name ()),
		string_literal (track.supercollider_synthdef ()),
		track.supercollider_source ()
	);
}

std::string
SuperColliderSessionRuntime::track_stop_code (SuperColliderTrack const& track) const
{
	return string_compose (
		"(\n"
		"var trackId = %1;\n"
		"var state = ~ardourTracks[trackId];\n"
		"if (state.notNil) {\n"
		"    if (state[\\group].notNil) {\n"
		"        state[\\group].freeAll;\n"
		"        state[\\group].free;\n"
		"    };\n"
		"    ~ardourTracks.removeAt(trackId);\n"
		"};\n"
		")\n",
		string_literal (track_key (track))
	);
}

void
SuperColliderSessionRuntime::runtime_output (std::string text, size_t len)
{
	if (len == 0 || text.empty ()) {
		return;
	}

	PBD::info << string_compose (_("SuperColliderSession: %1"), text.substr (0, len)) << endmsg;
}

void
SuperColliderSessionRuntime::runtime_terminated ()
{
	_runtime_connections.drop_connections ();
	_runtime.reset ();
	_active_tracks.clear ();
	_last_error = _("sclang terminated unexpectedly");
}
