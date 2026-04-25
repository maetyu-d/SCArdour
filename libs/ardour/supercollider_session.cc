#include "ardour/supercollider_session.h"

#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>

#include <glib.h>

#include "ardour/playlist.h"
#include "ardour/region.h"
#include "ardour/session.h"
#include "ardour/supercollider_track.h"
#include "ardour/system_exec.h"

#include "pbd/compose.h"
#include "pbd/error.h"

#include "pbd/i18n.h"

using namespace ARDOUR;

SuperColliderSessionRuntime::SuperColliderSessionRuntime (Session& session)
	: _session (session)
	, _transport_poll_source (0)
	, _last_transport_sample (std::numeric_limits<samplepos_t>::max ())
	, _last_transport_rolling (false)
{
	_session.TransportStateChange.connect_same_thread (*this, std::bind (&SuperColliderSessionRuntime::sync_transport, this));
	_session.PositionChanged.connect_same_thread (*this, std::bind (&SuperColliderSessionRuntime::poll_transport, this));
	_transport_poll_source = g_timeout_add (25, &SuperColliderSessionRuntime::transport_poll_cb, this);
}

SuperColliderSessionRuntime::~SuperColliderSessionRuntime ()
{
	if (_transport_poll_source != 0) {
		g_source_remove (_transport_poll_source);
		_transport_poll_source = 0;
	}

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

	_active_tracks[track_key (track)] = &track;
	_active_regions.erase (track_key (track));
	poll_transport ();
	_last_error.clear ();
	return true;
}

void
SuperColliderSessionRuntime::deactivate_track (SuperColliderTrack const& track)
{
	std::string const key = track_key (track);
	if (running () && _active_regions.find (key) != _active_regions.end ()) {
		send_code (track_stop_code (track));
	}

	_active_regions.erase (key);
	_active_tracks.erase (key);

	if (_active_tracks.empty ()) {
		stop ();
	}
}

void
SuperColliderSessionRuntime::stop ()
{
	_active_tracks.clear ();
	_active_regions.clear ();
	_runtime_connections.drop_connections ();
	_last_transport_sample = std::numeric_limits<samplepos_t>::max ();
	_last_transport_rolling = false;

	if (_runtime) {
		_runtime->terminate ();
		_runtime.reset ();
	}
}

void
SuperColliderSessionRuntime::sync_transport ()
{
	poll_transport ();
}

void
SuperColliderSessionRuntime::sync_transport_state ()
{
	if (!running ()) {
		return;
	}

	samplepos_t const sample = _session.transport_sample ();
	bool const rolling = _session.transport_state_rolling ();

	if (_last_transport_sample == sample && _last_transport_rolling == rolling) {
		return;
	}

	_last_transport_sample = sample;
	_last_transport_rolling = rolling;

	send_code (string_compose (
		"~ardourTransportRolling = %1;\n"
		"~ardourTransportSample = %2;\n",
		rolling ? "true" : "false",
		std::to_string (sample)
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

	sync_transport_state ();
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
SuperColliderSessionRuntime::track_play_region_code (SuperColliderTrack const& track, Region const& region) const
{
	return string_compose (
		"(\n"
		"var trackId = %1;\n"
		"var trackName = %2;\n"
		"var synthdefName = %3;\n"
		"var regionId = %4;\n"
		"var regionName = %5;\n"
		"var regionStart = %6;\n"
		"var regionEnd = %7;\n"
		"~ardourTracks = ~ardourTracks ? IdentityDictionary.new;\n"
		"s = Server.default ? Server.local;\n"
		"s.waitForBoot({\n"
		"    var state = ~ardourTracks[trackId];\n"
		"    var trackGroup;\n"
		"    if (state.notNil) {\n"
		"        if (state[\\group].notNil) {\n"
		"            state[\\group].freeAll;\n"
		"            state[\\group].free;\n"
		"        };\n"
		"    };\n"
		"    trackGroup = Group.tail(s);\n"
		"    ~ardourTrackId = trackId;\n"
		"    ~ardourTrackName = trackName;\n"
		"    ~ardourSynthDef = synthdefName;\n"
		"    ~ardourRegionId = regionId;\n"
		"    ~ardourRegionName = regionName;\n"
		"    ~ardourRegionStart = regionStart;\n"
		"    ~ardourRegionEnd = regionEnd;\n"
		"    ~ardourTrackGroup = trackGroup;\n"
		"    state = (name: trackName, synthdef: synthdefName, group: trackGroup, region: regionName, regionId: regionId, regionStart: regionStart, regionEnd: regionEnd);\n"
		"    ~ardourTracks[trackId] = state;\n"
		"%8\n"
		"});\n"
		")\n",
		string_literal (track_key (track)),
		string_literal (track.name ()),
		string_literal (track.supercollider_synthdef ()),
		string_literal (region.id ().to_s ()),
		string_literal (region.name ()),
		std::to_string (region.position_sample ()),
		std::to_string (region.position_sample () + region.length_samples ()),
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
		"    state[\\group] = nil;\n"
		"    state[\\region] = nil;\n"
		"    state[\\regionId] = nil;\n"
		"    ~ardourTracks[trackId] = state;\n"
		"};\n"
		")\n",
		string_literal (track_key (track))
	);
}

gboolean
SuperColliderSessionRuntime::transport_poll_cb (gpointer data)
{
	static_cast<SuperColliderSessionRuntime*> (data)->poll_transport ();
	return G_SOURCE_CONTINUE;
}

std::shared_ptr<Region>
SuperColliderSessionRuntime::active_region (SuperColliderTrack const& track, samplepos_t sample) const
{
	std::shared_ptr<Playlist> const pl = const_cast<SuperColliderTrack&> (track).playlist ();
	if (!pl) {
		return std::shared_ptr<Region> ();
	}

	return pl->top_unmuted_region_at (timepos_t (sample));
}

void
SuperColliderSessionRuntime::poll_transport ()
{
	if (!running ()) {
		return;
	}

	sync_transport_state ();

	if (!_session.transport_state_rolling ()) {
		for (std::map<std::string, SuperColliderTrack const*>::const_iterator i = _active_tracks.begin (); i != _active_tracks.end (); ++i) {
			if (_active_regions.find (i->first) != _active_regions.end ()) {
				send_code (track_stop_code (*i->second));
			}
		}
		_active_regions.clear ();
		return;
	}

	samplepos_t const now = _session.transport_sample ();

	for (std::map<std::string, SuperColliderTrack const*>::const_iterator i = _active_tracks.begin (); i != _active_tracks.end (); ++i) {
		std::shared_ptr<Region> const region = active_region (*i->second, now);
		std::string const next_region_id = region ? region->id ().to_s () : "";
		std::string const current_region_id = _active_regions[i->first];

		if (current_region_id == next_region_id) {
			continue;
		}

		if (!current_region_id.empty ()) {
			send_code (track_stop_code (*i->second));
		}

		if (region) {
			send_code (track_play_region_code (*i->second, *region));
			_active_regions[i->first] = next_region_id;
		} else {
			_active_regions.erase (i->first);
		}
	}
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
	_active_regions.clear ();
	_last_error = _("sclang terminated unexpectedly");
}
