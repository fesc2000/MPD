/*
 * Copyright 2003-2018 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config/Block.hxx"
#include "mixer/MixerInternal.hxx"
#include "util/Domain.hxx"
#include "Log.hxx"
#include <string.h>

static constexpr Domain volfile_mixer_domain("volfile_mixer");

class VolfileMixer final : public Mixer {
	/**
	 * The current volume in percent (0..100).
	 */
	unsigned last_volume;

	const char *volfile = "";

public:
	VolfileMixer(MixerListener &_listener)
		:Mixer(volfile_mixer_plugin, _listener),
		 last_volume(75)
	{
	}

	/* virtual methods from class Mixer */
	void Open() override {
	    	last_volume = GetVolume ();
	}

	void Close() noexcept override {
	}

	int GetVolume() override {
		int vol;

		if (strlen(volfile) == 0)
			return last_volume;

		FILE *fd = fopen (volfile, "r");
		if (fd == NULL)
		{
			FormatError (volfile_mixer_domain, "Failed to open volume control %s %d", volfile, errno);
			return last_volume;
	    	}

		if (fscanf (fd, "%d", &vol) != 1)
			return last_volume;

	    	fclose (fd);

	    	return (last_volume = vol);
	};

	void SetVolume(unsigned _volume) override {
	    	char out[20];

	    	if (_volume == last_volume)
			return;

	    	if (strlen(volfile) == 0)
			return;

	    	FILE *fd = fopen (volfile, "w+");
	    	if (fd == NULL)
	    	{
			FormatError (volfile_mixer_domain, "Failed to open volume control %s", volfile);
			return;
	    	}

		sprintf (out, "%d", _volume);
	    	if (fwrite (out, strlen(out), 1, fd) == 0)
	    	{
			FormatError (volfile_mixer_domain, "Failed to write volume control %s", volfile);
			return;
	    	}

	    	last_volume = _volume;

	    	fclose (fd);
	}

	void Configure(const ConfigBlock &block)
	{
		volfile = block.GetBlockValue("volume_file", "");
	}
};

static Mixer *
volfile_mixer_init(gcc_unused EventLoop &event_loop,
		gcc_unused AudioOutput &ao,
		MixerListener &listener,
		const ConfigBlock &block)
{
	VolfileMixer *mixer = new VolfileMixer(listener);
	mixer->Configure(block);
	return mixer;
}

const MixerPlugin volfile_mixer_plugin = {
	volfile_mixer_init,
	true,
};
