#pragma once

////////////////////////////////////////////////////////////////////////////////
// The MIT License (MIT)
//
// Copyright (c) 2017 Nicholas Frechette & Animation Compression Library contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
////////////////////////////////////////////////////////////////////////////////

#include "acl/compression/stream/clip_context.h"
#include "acl/compression/skeleton_error_metric.h"
#include "acl/decompression/functions.h"
#include "acl/sjson/sjson_writer.h"

namespace acl
{
	inline void write_summary_segment_stats(const SegmentContext& segment, RotationFormat8 rotation_format, VectorFormat8 translation_format, VectorFormat8 scale_format, SJSONObjectWriter& writer)
	{
		writer["segment_index"] = segment.segment_index;
		writer["num_samples"] = segment.num_samples;

		const uint32_t format_per_track_data_size = get_format_per_track_data_size(*segment.clip, rotation_format, translation_format, scale_format);

		uint32_t segment_size = 0;
		segment_size += format_per_track_data_size;						// Format per track data
		segment_size = align_to(segment_size, 2);						// Align range data
		segment_size += segment.range_data_size;						// Range data
		segment_size = align_to(segment_size, 4);						// Align animated data
		segment_size += segment.animated_data_size;						// Animated track data

		writer["segment_size"] = segment_size;
		writer["animated_frame_size"] = float(segment.animated_data_size) / float(segment.num_samples);
	}

	inline void write_detailed_segment_stats(const SegmentContext& segment, SJSONObjectWriter& writer)
	{
		uint32_t bit_rate_counts[NUM_BIT_RATES] = {0};

		for (const BoneStreams& bone_stream : segment.bone_iterator())
		{
			const uint8_t rotation_bit_rate = bone_stream.rotations.get_bit_rate();
			if (rotation_bit_rate != INVALID_BIT_RATE)
				bit_rate_counts[rotation_bit_rate]++;

			const uint8_t translation_bit_rate = bone_stream.translations.get_bit_rate();
			if (translation_bit_rate != INVALID_BIT_RATE)
				bit_rate_counts[translation_bit_rate]++;

			const uint8_t scale_bit_rate = bone_stream.scales.get_bit_rate();
			if (scale_bit_rate != INVALID_BIT_RATE)
				bit_rate_counts[scale_bit_rate]++;
		}

		writer["bit_rate_counts"] = [&](SJSONArrayWriter& writer)
		{
			for (uint8_t bit_rate = 0; bit_rate < NUM_BIT_RATES; ++bit_rate)
				writer.push_value(bit_rate_counts[bit_rate]);
		};
	}

	inline void write_exhaustive_segment_stats(Allocator& allocator, const SegmentContext& segment, const ClipContext& raw_clip_context, const RigidSkeleton& skeleton, SJSONObjectWriter& writer)
	{
		const uint16_t num_bones = skeleton.get_num_bones();
		const bool has_scale = segment_context_has_scale(segment);

		Transform_32* raw_local_pose = allocate_type_array<Transform_32>(allocator, num_bones);
		Transform_32* lossy_local_pose = allocate_type_array<Transform_32>(allocator, num_bones);

		const float sample_rate = float(raw_clip_context.segments[0].bone_streams[0].rotations.get_sample_rate());
		const float ref_duration = float(raw_clip_context.num_samples - 1) / sample_rate;

		const float segment_duration = float(segment.num_samples - 1) / sample_rate;

		BoneError worst_bone_error = { INVALID_BONE_INDEX, 0.0f, 0.0f };

		writer["error_per_frame_and_bone"] = [&](SJSONArrayWriter& writer)
		{
			for (uint32_t sample_index = 0; sample_index < segment.num_samples; ++sample_index)
			{
				const float sample_time = min(float(sample_index) / sample_rate, segment_duration);
				const float ref_sample_time = min(float(segment.clip_sample_offset + sample_index) / sample_rate, ref_duration);

				sample_streams(raw_clip_context.segments[0].bone_streams, num_bones, ref_sample_time, raw_local_pose);
				sample_streams(segment.bone_streams, num_bones, sample_time, lossy_local_pose);

				writer.push_newline();
				writer.push_array([&](SJSONArrayWriter& writer)
				{
					for (uint16_t bone_index = 0; bone_index < num_bones; ++bone_index)
					{
						float error;
						if (has_scale)
							error = calculate_object_bone_error(skeleton, raw_local_pose, lossy_local_pose, bone_index);
						else
							error = calculate_object_bone_error_no_scale(skeleton, raw_local_pose, lossy_local_pose, bone_index);
						writer.push_value(error);

						if (error > worst_bone_error.error)
						{
							worst_bone_error.error = error;
							worst_bone_error.index = bone_index;
							worst_bone_error.sample_time = sample_time;
						}
					}
				});
			}
		};

		writer["max_error"] = worst_bone_error.error;
		writer["worst_bone"] = worst_bone_error.index;
		writer["worst_time"] = worst_bone_error.sample_time;

		deallocate_type_array(allocator, raw_local_pose, num_bones);
		deallocate_type_array(allocator, lossy_local_pose, num_bones);
	}

	inline void write_decompression_times(Allocator& allocator, const AnimationClip& clip, SJSONObjectWriter& writer,
		AllocateDecompressionContext allocate_context, DecompressPose decompress_pose, DeallocateDecompressionContext deallocate_context)
	{
		uint16_t num_bones = clip.get_num_bones();
		float clip_duration = clip.get_duration();
		float sample_rate = float(clip.get_sample_rate());
		uint32_t num_samples = calculate_num_samples(clip_duration, clip.get_sample_rate());

		void* context = allocate_context(allocator);
		Transform_32* lossy_pose_transforms = allocate_type_array<Transform_32>(allocator, num_bones);

		{
			flush_data_cache(allocator);

			ScopeProfiler timer;

			for (uint32_t sample_index = 0; sample_index < num_samples; ++sample_index)
			{
				float sample_time = min(float(sample_index) / sample_rate, clip_duration);
				decompress_pose(context, sample_time, lossy_pose_transforms, num_bones);
			}

			timer.stop();

			writer["forward_decompression_time"] = timer.get_elapsed_seconds();
		}

		{
			flush_data_cache(allocator);

			ScopeProfiler timer;

			for (int32_t sample_index = num_samples - 1; sample_index >= 0; --sample_index)
			{
				float sample_time = min(float(sample_index) / sample_rate, clip_duration);
				decompress_pose(context, sample_time, lossy_pose_transforms, num_bones);
			}

			timer.stop();

			writer["backward_decompression_time"] = timer.get_elapsed_seconds();
		}

		deallocate_type_array(allocator, lossy_pose_transforms, num_bones);
		deallocate_context(allocator, context);
	}

	inline void write_stats(Allocator& allocator, const AnimationClip& clip, const ClipContext& clip_context, const RigidSkeleton& skeleton,
		const CompressedClip& compressed_clip, const CompressionSettings& settings, const ClipHeader& header, const ClipContext& raw_clip_context, const ScopeProfiler& compression_time,
		OutputStats& stats, AllocateDecompressionContext allocate_context, DecompressPose decompress_pose, DeallocateDecompressionContext deallocate_context)
	{
		uint32_t raw_size = clip.get_raw_size();
		uint32_t compressed_size = compressed_clip.get_size();
		double compression_ratio = double(raw_size) / double(compressed_size);

		// Use the compressed clip to make sure the decoder works properly
		BoneError error = calculate_compressed_clip_error(allocator, clip, skeleton, clip_context.has_scale, allocate_context, decompress_pose, deallocate_context);

		SJSONObjectWriter& writer = stats.get_writer();
		writer["algorithm_name"] = get_algorithm_name(AlgorithmType8::UniformlySampled);
		writer["algorithm_uid"] = settings.hash();
		writer["clip_name"] = clip.get_name().c_str();
		writer["raw_size"] = raw_size;
		writer["compressed_size"] = compressed_size;
		writer["compression_ratio"] = compression_ratio;
		writer["max_error"] = error.error;
		writer["worst_bone"] = error.index;
		writer["worst_time"] = error.sample_time;
		writer["compression_time"] = compression_time.get_elapsed_seconds();
		writer["duration"] = clip.get_duration();
		writer["num_samples"] = clip.get_num_samples();
		writer["rotation_format"] = get_rotation_format_name(settings.rotation_format);
		writer["translation_format"] = get_vector_format_name(settings.translation_format);
		writer["scale_format"] = get_vector_format_name(settings.scale_format);
		writer["range_reduction"] = get_range_reduction_name(settings.range_reduction);
		writer["has_scale"] = clip_context.has_scale;

		write_decompression_times(allocator, clip, writer, allocate_context, decompress_pose, deallocate_context);

		if (stats.get_logging() == StatLogging::Detailed || stats.get_logging() == StatLogging::Exhaustive)
		{
			writer["num_bones"] = clip.get_num_bones();

			uint32_t num_default_rotation_tracks = 0;
			uint32_t num_default_translation_tracks = 0;
			uint32_t num_default_scale_tracks = 0;
			uint32_t num_constant_rotation_tracks = 0;
			uint32_t num_constant_translation_tracks = 0;
			uint32_t num_constant_scale_tracks = 0;
			uint32_t num_animated_rotation_tracks = 0;
			uint32_t num_animated_translation_tracks = 0;
			uint32_t num_animated_scale_tracks = 0;

			for (const BoneStreams& bone_stream : clip_context.segments[0].bone_iterator())
			{
				if (bone_stream.is_rotation_default)
					num_default_rotation_tracks++;
				else if (bone_stream.is_rotation_constant)
					num_constant_rotation_tracks++;
				else
					num_animated_rotation_tracks++;

				if (bone_stream.is_translation_default)
					num_default_translation_tracks++;
				else if (bone_stream.is_translation_constant)
					num_constant_translation_tracks++;
				else
					num_animated_translation_tracks++;

				if (bone_stream.is_scale_default)
					num_default_scale_tracks++;
				else if (bone_stream.is_scale_constant)
					num_constant_scale_tracks++;
				else
					num_animated_scale_tracks++;
			}

			uint32_t num_default_tracks = num_default_rotation_tracks + num_default_translation_tracks + num_default_scale_tracks;
			uint32_t num_constant_tracks = num_constant_rotation_tracks + num_constant_translation_tracks + num_constant_scale_tracks;
			uint32_t num_animated_tracks = num_animated_rotation_tracks + num_animated_translation_tracks + num_animated_scale_tracks;

			writer["num_default_rotation_tracks"] = num_default_rotation_tracks;
			writer["num_default_translation_tracks"] = num_default_translation_tracks;
			writer["num_default_scale_tracks"] = num_default_scale_tracks;

			writer["num_constant_rotation_tracks"] = num_constant_rotation_tracks;
			writer["num_constant_translation_tracks"] = num_constant_translation_tracks;
			writer["num_constant_scale_tracks"] = num_constant_scale_tracks;

			writer["num_animated_rotation_tracks"] = num_animated_rotation_tracks;
			writer["num_animated_translation_tracks"] = num_animated_translation_tracks;
			writer["num_animated_scale_tracks"] = num_animated_scale_tracks;

			writer["num_default_tracks"] = num_default_tracks;
			writer["num_constant_tracks"] = num_constant_tracks;
			writer["num_animated_tracks"] = num_animated_tracks;
		}

		if (settings.segmenting.enabled)
		{
			writer["segmenting"] = [&](SJSONObjectWriter& writer)
			{
				writer["num_segments"] = header.num_segments;
				writer["range_reduction"] = get_range_reduction_name(settings.segmenting.range_reduction);
				writer["ideal_num_samples"] = settings.segmenting.ideal_num_samples;
				writer["max_num_samples"] = settings.segmenting.max_num_samples;
			};
		}

		writer["segments"] = [&](SJSONArrayWriter& writer)
		{
			for (const SegmentContext& segment : clip_context.segment_iterator())
			{
				writer.push_object([&](SJSONObjectWriter& writer)
				{
					write_summary_segment_stats(segment, settings.rotation_format, settings.translation_format, settings.scale_format, writer);

					if (stats.get_logging() == StatLogging::Detailed || stats.get_logging() == StatLogging::Exhaustive)
					{
						write_detailed_segment_stats(segment, writer);
					}

					if (stats.get_logging() == StatLogging::Exhaustive)
					{
						write_exhaustive_segment_stats(allocator, segment, raw_clip_context, skeleton, writer);
					}
				});
			}
		};
	}
}
