#ifndef COMMON_RECEIVER_ROOM_HLSLI
#define COMMON_RECEIVER_ROOM_HLSLI

namespace ReceiverRoom
{
	static const uint MaximumRooms = 128;
	static const uint GenerationMask = 255;
	static const float MetadataScale = 65535.0;
	static const uint Unknown = 1;
	static const uint NoRoom = 2;
	static const uint FirstRoom = 3;
	static const uint MissingCoverage = FirstRoom + MaximumRooms;
	static const uint MissingGeneration = MissingCoverage + 1;
	static const uint StaleGeneration = MissingCoverage + 2;
	static const uint MissingCode = MissingCoverage + 3;
	static const uint UnwrittenMetadata = MissingCoverage + 4;
	static const uint PartialCoverageWithCode = MissingCoverage + 5;
	static const uint InvalidCode = MissingCoverage + 6;

	/** @brief Packs a known native room classification and current table generation into UNORM16. */
	float Encode(int room, uint generation)
	{
		if (room < -1 || room >= int(MaximumRooms) || generation == 0 || generation > GenerationMask) return 0;
		return float((generation << 8) | uint(room + 2)) / MetadataScale;
	}

	/** @brief Rejects missing, partial-coverage or differently stamped receiver metadata. */
	uint Decode(float4 metadata, uint generation)
	{
		if (metadata.w != 1) {
			if (metadata.w == 0 && metadata.z == 0) return UnwrittenMetadata;
			return metadata.z != 0 ? PartialCoverageWithCode : MissingCoverage;
		}
		if (generation == 0 || generation > GenerationMask) return MissingGeneration;
		if (!isfinite(metadata.z) || metadata.z < 0 || metadata.z > 1) return InvalidCode;
		uint packed = uint(round(saturate(metadata.z) * MetadataScale));
		uint room = packed & GenerationMask;
		if (packed == 0) return MissingCode;
		if (room == 0 || room > MaximumRooms + 1) return InvalidCode;
		if ((packed >> 8) != generation) return StaleGeneration;
		return room + 1;
	}

	/** @brief Returns unknown (-1), excluded (0) or admitted (1), preserving native no-room policy. */
	int Admission(float receiverCode, uint4 rooms)
	{
		uint code = uint(round(receiverCode));
		if (code == NoRoom) return 1;
		if (code < FirstRoom || code >= FirstRoom + MaximumRooms) return -1;
		uint room = code - FirstRoom;
		return int((rooms[room >> 5] >> (room & 31)) & 1);
	}
}

#endif
