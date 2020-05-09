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

#include "RecorderOutputPlugin.hxx"
#include "../OutputAPI.hxx"
#include "tag/Format.hxx"
#include "encoder/ToOutputStream.hxx"
#include "encoder/EncoderInterface.hxx"
#include "encoder/Configured.hxx"
#include "config/Domain.hxx"
#include "config/Path.hxx"
#include "Log.hxx"
#include "fs/AllocatedPath.hxx"
#include "fs/io/FileOutputStream.hxx"
#include "fs/io/FileReader.hxx"
#include "util/Domain.hxx"
#include "util/ScopeExit.hxx"
#include "thread/Thread.hxx"
#include "thread/Name.hxx"

#include <stdexcept>
#include <memory>

#include <assert.h>
#include <stdlib.h>

static constexpr Domain recorder_domain("recorder");

class RecorderOutput final : AudioOutput {
	/**
	 * The configured encoder plugin.
	 */
	std::unique_ptr<PreparedEncoder> prepared_encoder;
	Encoder *encoder;

	/**
	 * The destination file name.
	 */
	AllocatedPath path = nullptr;

	/**
	 * A string that will be used with FormatTag() to build the
	 * destination path.
	 */
	std::string format_path;

	/**
	 * The archive destination file name.
	 */
	AllocatedPath archive_path = nullptr;

	/**
	 * The #AudioFormat that is currently active.  This is used
	 * for switching to another file.
	 */
	AudioFormat effective_audio_format;

	/**
	 * The destination file.
	 */
	FileOutputStream *file;

	RecorderOutput(const ConfigBlock &block);

public:
	static AudioOutput *Create(EventLoop &, const ConfigBlock &block) {
		return new RecorderOutput(block);
	}

private:
	void Open(AudioFormat &audio_format) override;
	void Close() noexcept override;
	void SetAttribute(std::string &&name, std::string &&value) override;

	/**
	 * Writes pending data from the encoder to the output file.
	 */
	void EncoderToFile();

	void SendTag(const Tag &tag) override;

	size_t Play(const void *chunk, size_t size) override;

private:
	gcc_pure
	bool HasDynamicPath() const noexcept {
		return !format_path.empty();
	}

	/**
	 * Finish the encoder and commit the file.
	 *
	 * Throws #std::runtime_error on error.
	 */
	void Commit();

	void FinishFormat();
	void ReopenFormat(AllocatedPath &&new_path);
	
	void ArchiveTask() noexcept;

	bool archive_requested = false;
	bool delete_after_record = false;
	std::string archive_format_path;

	/**
	 * Thread to copy/move output file to archive.
	 * archive source/dest are the parameters
	 */
	Thread archive_thread;
	std::string archive_source = "";
	std::string archive_dest = "";

	/**
	 * hack for old "parent" property.
	 */
	const char *parent = nullptr;
	const char *output_name = nullptr;
};

RecorderOutput::RecorderOutput(const ConfigBlock &block)
	:AudioOutput(0),
	 prepared_encoder(CreateConfiguredEncoder(block)),
	 archive_thread(BIND_THIS_METHOD(ArchiveTask))
{
	/* read configuration */

	parent = block.GetBlockValue("parent", nullptr);
	output_name = block.GetBlockValue("name", nullptr);
	if (parent != nullptr) {
		return;
	}
 
	path = block.GetPath("path");

	const char *fmt = block.GetBlockValue("format_path", nullptr);
	if (fmt != nullptr)
		format_path = fmt;

	if (path.IsNull() && fmt == nullptr)
		throw std::runtime_error("'path' not configured");

	if (!path.IsNull() && fmt != nullptr)
		throw std::runtime_error("Cannot have both 'path' and 'format_path'");

	const char *archive_fmt = block.GetBlockValue("archive_path", nullptr);
	if (archive_fmt != nullptr) {
		archive_format_path = archive_fmt;
	}

	if (block.GetBlockValue("delete_after_record", nullptr))
		delete_after_record = true;
}

inline void
RecorderOutput::EncoderToFile()
{
	assert(file != nullptr);

	EncoderToOutputStream(*file, *encoder);
}

void
RecorderOutput::Open(AudioFormat &audio_format)
{
	if (parent != nullptr) {
		std::string cmd;

		cmd = "(/usr/local/bin/mpc outputset '";
		cmd += parent;
		cmd += "' archive=1; ";
		if (output_name != nullptr) {
			cmd += "/usr/local/bin/mpc disable '";
			cmd += output_name;
			cmd += "'";
		}
		cmd += ")&";

		FormatDebug(recorder_domain, "Archive Cmd: %s", cmd.c_str());

		system (cmd.c_str());

		return;
	}

	/* create the output file */

	if (!HasDynamicPath()) {
		assert(!path.IsNull());

		file = new FileOutputStream(path);
	} else {
		/* don't open the file just yet; wait until we have
		   a tag that we can use to build the path */
		assert(path.IsNull());

		file = nullptr;
	}

	/* open the encoder */

	try {
		encoder = prepared_encoder->Open(audio_format);
	} catch (...) {
		delete file;
		throw;
	}

	if (!HasDynamicPath()) {
		try {
			EncoderToFile();
		} catch (...) {
			delete encoder;
			throw;
		}
	} else {
		/* remember the AudioFormat for ReopenFormat() */
		effective_audio_format = audio_format;

		/* close the encoder for now; it will be opened as
		   soon as we have received a tag */
		delete encoder;
	}
}

inline void
RecorderOutput::Commit()
{
	if (parent != nullptr)
		return;

	assert(!path.IsNull());

	/* flush the encoder and write the rest to the file */

	try {
		encoder->End();
		EncoderToFile();
	} catch (...) {
		delete encoder;
		throw;
	}

	/* now really close everything */

	delete encoder;

	try {
		file->Commit();
	} catch (...) {
		delete file;
		throw;
	}

	/* move file to archive if requested */
	if (archive_requested && !archive_path.IsNull()) {

		// wait for previous copy/move operation to finish
		if (archive_thread.IsDefined())
			archive_thread.Join();

		archive_source = path.c_str();
		archive_dest = archive_path.c_str();
		archive_thread.Start();
		archive_requested = false;
	}
	else
	{
	    /* delete file if requested */
	    if (delete_after_record) {		    
		    if (remove(path.c_str())) {
			FormatError(recorder_domain, "Failed to remove \"%s\"",
				path.c_str());
		    } else {
			    FormatDebug(recorder_domain, "Removed \"%s\"", path.c_str());
		    }
	    }
	}

	delete file;
}

void
RecorderOutput::ArchiveTask() noexcept {
	bool success = true;

	SetThreadName("archive_file");

	if (delete_after_record) {
		if (rename (archive_source.c_str(), archive_dest.c_str())) {
			FormatError(recorder_domain, "Failed to move \"%s\" to \"%s\"",
				archive_source.c_str(), archive_dest.c_str());
			success = false;
		}
	} else {
		try {
			FileReader source(Path::FromFS(archive_source.c_str()));
			FileOutputStream dest(Path::FromFS(archive_dest.c_str()));

			char buffer[256];
			size_t nbytes;

			while ((nbytes = source.Read(buffer, sizeof(buffer))) > 0) {
				dest.Write(buffer, nbytes);
			}
			dest.Commit();
		} 
		catch(...)
		{
			FormatError(recorder_domain, "Failed to copy \"%s\" to \"%s\"",
				archive_source.c_str(), archive_dest.c_str());
			LogError(std::current_exception());
			success = false;
		}
	}

	if (success) {
		FormatDebug(recorder_domain, "%s \"%s\" to \"%s\"",
			delete_after_record ? "Moved" : "Copied",
			archive_source.c_str(), archive_dest.c_str());
	}
}

void
RecorderOutput::Close() noexcept
{
	if (parent != nullptr)
		return;	

	if (file == nullptr) {
		/* not currently encoding to a file; nothing needs to
		   be done now */
		assert(HasDynamicPath());
		assert(path.IsNull());
		return;
	}

	try {
		Commit();
	} catch (...) {
		LogError(std::current_exception());
	}

	if (HasDynamicPath()) {
		assert(!path.IsNull());
		path.SetNull();
	}
}

void
RecorderOutput::FinishFormat()
{
	if (parent != nullptr) {
		return;	
	}

	assert(HasDynamicPath());

	if (file == nullptr)
		return;

	try {
		Commit();
	} catch (...) {
		LogError(std::current_exception());
	}

	file = nullptr;
	path.SetNull();
}

inline void
RecorderOutput::ReopenFormat(AllocatedPath &&new_path)
{
	assert(HasDynamicPath());
	assert(path.IsNull());
	assert(file == nullptr);

	FileOutputStream *new_file = new FileOutputStream(new_path);

	AudioFormat new_audio_format = effective_audio_format;

	try {
		encoder = prepared_encoder->Open(new_audio_format);
	} catch (...) {
		delete new_file;
		throw;
	}

	/* reopening the encoder must always result in the same
	   AudioFormat as before */
	assert(new_audio_format == effective_audio_format);

	try {
		EncoderToOutputStream(*new_file, *encoder);
	} catch (...) {
		delete encoder;
		delete new_file;
		throw;
	}

	path = std::move(new_path);
	file = new_file;

	FormatDebug(recorder_domain, "Recording to \"%s\"",
		    path.ToUTF8().c_str());
}

void
RecorderOutput::SendTag(const Tag &tag)
{
	if (parent != nullptr)
		return;

	if (HasDynamicPath()) {
		char *p = FormatTag(tag, format_path.c_str());
		if (p == nullptr || *p == 0) {
			/* no path could be composed with this tag:
			   don't write a file */
			free(p);
			FinishFormat();
			return;
		}

		AtScopeExit(p) { free(p); };

		AllocatedPath new_path = nullptr;

		try {
			new_path = ParsePath(p);
		} catch (...) {
			LogError(std::current_exception());
			FinishFormat();
			return;
		}

		if (new_path != path) {
			FinishFormat();

			try {
				ReopenFormat(std::move(new_path));
			} catch (...) {
				LogError(std::current_exception());
				return;
			}
		}

		/* Commit() will use archive_path to decide whether to crchive the
		   current output file */
		archive_path.SetNull();
		if (!archive_format_path.empty()) {
			char *ap = FormatTag(tag, archive_format_path.c_str());
			AtScopeExit(ap) { free(ap); };

			try {
				archive_path = ParsePath(ap);
			} catch (const std::runtime_error &e) {
				LogError(e);
			}	
		}
	}

	encoder->PreTag();
	EncoderToFile();
	encoder->SendTag(tag);
}

size_t
RecorderOutput::Play(const void *chunk, size_t size)
{
	if (parent != nullptr)
		return size;

	if (file == nullptr) {
		/* not currently encoding to a file; discard incoming
		   data */
		assert(HasDynamicPath());
		assert(path.IsNull());
		return size;
	}

	encoder->Write(chunk, size);

	EncoderToFile();

	return size;
}

void
RecorderOutput::SetAttribute(std::string &&name, std::string &&value)
{
	if (parent != nullptr)
		return;

	if (name == "archive") {
		if (!archive_format_path.empty()) {
			archive_requested = (std::stoi(value) != 0);
			FormatDebug(recorder_domain, "archive_requested=%d", archive_requested);
		} else {
			FormatError(recorder_domain, "archive attribute set, "
						     "but no archive_path configured");
		}	
	}
}

const struct AudioOutputPlugin recorder_output_plugin = {
	"recorder",
	nullptr,
	&RecorderOutput::Create,
	nullptr,
};
