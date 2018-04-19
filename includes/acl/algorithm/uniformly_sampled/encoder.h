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

#include "acl/core/iallocator.h"
#include "acl/core/bitset.h"
#include "acl/core/error.h"
#include "acl/core/error_result.h"
#include "acl/core/algorithm_types.h"
#include "acl/core/track_types.h"
#include "acl/core/range_reduction_types.h"
#include "acl/core/scope_profiler.h"
#include "acl/compression/compressed_clip_impl.h"
#include "acl/compression/skeleton.h"
#include "acl/compression/animation_clip.h"
#include "acl/compression/output_stats.h"
#include "acl/compression/stream/clip_context.h"
#include "acl/compression/stream/track_stream.h"
#include "acl/compression/stream/convert_rotation_streams.h"
#include "acl/compression/stream/compact_constant_streams.h"
#include "acl/compression/stream/normalize_streams.h"
#include "acl/compression/stream/quantize_streams.h"
#include "acl/compression/stream/segment_streams.h"
#include "acl/compression/stream/write_segment_data.h"
#include "acl/compression/stream/write_stats.h"
#include "acl/compression/stream/write_stream_bitsets.h"
#include "acl/compression/stream/write_stream_data.h"
#include "acl/decompression/default_output_writer.h"

#include <cstdint>

namespace acl
{
	namespace uniformly_sampled
	{
		//////////////////////////////////////////////////////////////////////////
		// Compresses a raw clip with uniform sampling
		//
		// This compression algorithm is the simplest by far and as such it offers
		// the fastest compression and decompression. Every sample is retained and
		// every track has the same number of samples playing back at the same
		// sample rate. This means that when we sample at a particular time within
		// the clip, we can trivially calculate the offsets required to read the
		// desired data. All the data is sorted in order to ensure all reads are
		// as contiguous as possible for optimal cache locality during decompression.
		//
		//    allocator: The allocator instance to use to allocate and free memory
		//    clip: The raw clip to compress
		//    settings: The compression settings to use
		//    out_compressed_clip: The resulting compressed clip. The caller owns the returned memory and must free it
		//    out_stats: Stat output structure
		//////////////////////////////////////////////////////////////////////////
		inline ErrorResult compress_clip(IAllocator& allocator, const AnimationClip& clip, CompressionSettings settings, CompressedClip*& out_compressed_clip, OutputStats& out_stats)
		{
			using namespace impl;

			ErrorResult error_result = clip.is_valid();
			if (error_result.any())
				return error_result;

			error_result = settings.is_valid();
			if (error_result.any())
				return error_result;

			ScopeProfiler compression_time;

			const uint16_t num_bones = clip.get_num_bones();
			const uint32_t num_samples = clip.get_num_samples();
			const RigidSkeleton& skeleton = clip.get_skeleton();

			ClipContext additive_base_clip_context;
			const AnimationClip* additive_base_clip = clip.get_additive_base();
			if (additive_base_clip != nullptr)
				initialize_clip_context(allocator, *additive_base_clip, skeleton, settings, additive_base_clip_context);

			ClipContext raw_clip_context;
			initialize_clip_context(allocator, clip, skeleton, settings, raw_clip_context);

			ClipContext clip_context;
			initialize_clip_context(allocator, clip, skeleton, settings, clip_context);

			convert_rotation_streams(allocator, clip_context, settings.rotation_format);

			// Extract our clip ranges now, we need it for compacting the constant streams
			extract_clip_bone_ranges(allocator, clip_context);

			// Compact and collapse the constant streams
			compact_constant_streams(allocator, clip_context, settings.constant_rotation_threshold_angle, settings.constant_translation_threshold, settings.constant_scale_threshold);

			uint32_t clip_range_data_size = 0;
			if (settings.range_reduction != RangeReductionFlags8::None)
			{
				normalize_clip_streams(clip_context, settings.range_reduction);
				clip_range_data_size = get_stream_range_data_size(clip_context, settings.range_reduction, settings.rotation_format, settings.translation_format, settings.scale_format);
			}

			if (settings.segmenting.enabled)
			{
				segment_streams(allocator, clip_context, settings.segmenting);

				// If we have a single segment, disable range reduction since it won't help
				if (clip_context.num_segments == 1)
					settings.segmenting.range_reduction = RangeReductionFlags8::None;

				if (settings.segmenting.range_reduction != RangeReductionFlags8::None)
				{
					extract_segment_bone_ranges(allocator, clip_context);
					normalize_segment_streams(clip_context, settings.range_reduction);
				}
			}

			quantize_streams(allocator, clip_context, settings, skeleton, raw_clip_context, additive_base_clip_context);

			const uint32_t constant_data_size = get_constant_data_size(clip_context);

			calculate_animated_data_size(clip_context, settings.rotation_format, settings.translation_format, settings.scale_format);

			const uint32_t format_per_track_data_size = get_format_per_track_data_size(clip_context, settings.rotation_format, settings.translation_format, settings.scale_format);

			const uint32_t num_tracks_per_bone = clip_context.has_scale ? 3 : 2;
			const uint32_t num_tracks = uint32_t(num_bones) * num_tracks_per_bone;
			const BitSetDescription bitset_desc = BitSetDescription::make_from_num_bits(num_tracks);

			uint32_t buffer_size = 0;
			// Per clip data
			buffer_size += sizeof(CompressedClip);
			buffer_size += sizeof(ClipHeader);
			buffer_size += sizeof(SegmentHeader) * clip_context.num_segments;	// Segment headers
			buffer_size = align_to(buffer_size, 4);								// Align bitsets
			buffer_size += bitset_desc.get_num_bytes();							// Default tracks bitset
			buffer_size += bitset_desc.get_num_bytes();							// Constant tracks bitset
			buffer_size = align_to(buffer_size, 4);								// Align constant track data
			buffer_size += constant_data_size;									// Constant track data
			buffer_size = align_to(buffer_size, 4);								// Align range data
			buffer_size += clip_range_data_size;								// Range data

			clip_context.total_header_size = buffer_size;

			// Per segment data
			for (SegmentContext& segment : clip_context.segment_iterator())
			{
				const uint32_t header_start = buffer_size;

				buffer_size += format_per_track_data_size;						// Format per track data
				// TODO: Alignment only necessary with 16bit per component
				buffer_size = align_to(buffer_size, 2);							// Align range data
				buffer_size += segment.range_data_size;							// Range data

				const uint32_t header_end = buffer_size;

				// TODO: Variable bit rate doesn't need alignment
				buffer_size = align_to(buffer_size, 4);							// Align animated data
				buffer_size += segment.animated_data_size;						// Animated track data

				segment.total_header_size = header_end - header_start;
			}

			uint8_t* buffer = allocate_type_array_aligned<uint8_t>(allocator, buffer_size, 16);

			CompressedClip* compressed_clip = make_compressed_clip(buffer, buffer_size, AlgorithmType8::UniformlySampled);

			ClipHeader& header = get_clip_header(*compressed_clip);
			header.num_bones = num_bones;
			header.num_segments = clip_context.num_segments;
			header.rotation_format = settings.rotation_format;
			header.translation_format = settings.translation_format;
			header.scale_format = settings.scale_format;
			header.clip_range_reduction = settings.range_reduction;
			header.segment_range_reduction = settings.segmenting.range_reduction;
			header.has_scale = clip_context.has_scale ? 1 : 0;
			header.default_scale = additive_base_clip == nullptr || clip.get_additive_format() != AdditiveClipFormat8::Additive1;
			header.num_samples = num_samples;
			header.sample_rate = clip.get_sample_rate();
			header.segment_headers_offset = sizeof(ClipHeader);
			header.default_tracks_bitset_offset = align_to(header.segment_headers_offset + (sizeof(SegmentHeader) * clip_context.num_segments), 4);
			header.constant_tracks_bitset_offset = header.default_tracks_bitset_offset + bitset_desc.get_num_bytes();
			header.constant_track_data_offset = align_to(header.constant_tracks_bitset_offset + bitset_desc.get_num_bytes(), 4);
			header.clip_range_data_offset = align_to(header.constant_track_data_offset + constant_data_size, 4);

			const uint16_t segment_headers_start_offset = safe_static_cast<uint16_t>(header.clip_range_data_offset + clip_range_data_size);
			write_segment_headers(clip_context, settings, header.get_segment_headers(), segment_headers_start_offset);
			write_default_track_bitset(clip_context, header.get_default_tracks_bitset(), bitset_desc);
			write_constant_track_bitset(clip_context, header.get_constant_tracks_bitset(), bitset_desc);

			if (constant_data_size > 0)
				write_constant_track_data(clip_context, header.get_constant_track_data(), constant_data_size);
			else
				header.constant_track_data_offset = InvalidPtrOffset();

			if (settings.range_reduction != RangeReductionFlags8::None)
				write_clip_range_data(clip_context, settings.range_reduction, header.get_clip_range_data(), clip_range_data_size);
			else
				header.clip_range_data_offset = InvalidPtrOffset();

			write_segment_data(clip_context, settings, header);

			finalize_compressed_clip(*compressed_clip);

			compression_time.stop();

#if defined(SJSON_CPP_WRITER)
			if (out_stats.logging != StatLogging::None)
				write_stats(allocator, clip, clip_context, skeleton, *compressed_clip, settings, header, raw_clip_context, additive_base_clip_context, compression_time, out_stats);
#endif

			destroy_clip_context(allocator, clip_context);
			destroy_clip_context(allocator, raw_clip_context);

			if (additive_base_clip != nullptr)
				destroy_clip_context(allocator, additive_base_clip_context);

			out_compressed_clip = compressed_clip;
			return ErrorResult();
		}
	}
}
