/*-------------------------------------------------------------------------
 *
 * tuple.cpp
 * file description
 *
 * Copyright(c) 2015, CMU
 *
 * /n-store/src/storage/abstract_tuple.cpp
 *
 *-------------------------------------------------------------------------
 */

#include "storage/tuple.h"

#include <cstdlib>
#include <sstream>
#include <cassert>

#include "storage/tuple.h"
#include "common/exception.h"

namespace nstore {
namespace storage {

// Set all columns by value into this tuple.
void Tuple::SetValueAllocate(const id_t column_id,
		Value value, Pool *dataPool) {
	assert(tuple_schema);
	assert(tuple_data);

	const ValueType type = tuple_schema->GetType(column_id);
	value = value.CastAs(type);

	const bool is_inlined = tuple_schema->IsInlined(column_id);
	char *dataPtr = GetDataPtr(column_id);
	int32_t column_length = tuple_schema->GetLength(column_id);

	if(is_inlined == false)
		column_length = tuple_schema->GetVariableLength(column_id);

	value.SerializeWithAllocation(dataPtr, is_inlined, column_length, dataPool);
}

// For an insert, the copy should do an allocation for all uninlinable columns
// This does not do any schema checks. They must match.
void Tuple::Copy(const void *source, Pool *pool) {
	assert(tuple_schema);
	assert(tuple_data);

	const bool is_inlined = tuple_schema->IsInlined();
	const id_t uninlineable_column_count = tuple_schema->GetUninlinedColumnCount();

	if (is_inlined) {
		// copy the data
		::memcpy(tuple_data, source, tuple_schema->GetLength());
	} else {
		// copy the data
		::memcpy(tuple_data, source, tuple_schema->GetLength());

		// Copy each uninlined column doing an allocation for copies.
		for (id_t column_itr = 0; column_itr < uninlineable_column_count; column_itr++) {
			const id_t unlineable_column_id =
					tuple_schema->GetUninlinedColumnIndex(column_itr);

			// Get original value from uninlined pool
			Value value = GetValue(unlineable_column_id);

			// Make a copy of the value at a new location in uninlined pool
			SetValueAllocate(unlineable_column_id, value, pool);
		}

	}

}

/**
 * Determine the maximum number of bytes when serialized for Export.
 * Excludes the bytes required by the row header (which includes
 * the null bit indicators) and ignores the width of metadata columns.
 */
size_t Tuple::ExportSerializationSize() const {
	size_t bytes = 0;
	int column_count = GetColumnCount();

	for (int column_itr = 0; column_itr < column_count; ++column_itr) {
		switch (GetType(column_itr)) {
		case VALUE_TYPE_TINYINT:
		case VALUE_TYPE_SMALLINT:
		case VALUE_TYPE_INTEGER:
		case VALUE_TYPE_BIGINT:
		case VALUE_TYPE_TIMESTAMP:
		case VALUE_TYPE_DOUBLE:
			bytes += sizeof(int64_t);
			break;

		case VALUE_TYPE_DECIMAL:
			// Decimals serialized in ascii as
			// 32 bits of length + max prec digits + radix pt + sign
			bytes += sizeof(int32_t) + Value::max_decimal_precision + 1 + 1;
			break;

		case VALUE_TYPE_VARCHAR:
		case VALUE_TYPE_VARBINARY:
			// 32 bit length preceding value and
			// actual character data without null string terminator.
			if (!GetValue(column_itr).IsNull()) {
				bytes += (sizeof(int32_t)
						+ ValuePeeker::PeekObjectLength(
								GetValue(column_itr)));
			}
			break;

		default:
			throw UnknownTypeException(GetType(column_itr),
					"Unknown ValueType found during Export serialization.");
			return (size_t) 0;
		}
	}
	return bytes;
}


// Return the amount of memory allocated for non-inlined objects
size_t Tuple::GetUninlinedMemorySize() const {
	size_t bytes = 0;
	int column_count = GetColumnCount();

	// fast-path for no inlined cols
	if (tuple_schema->IsInlined() == false) {
		for (int column_itr = 0; column_itr < column_count; ++column_itr) {
			// peekObjectLength is unhappy with non-varchar
			if ((GetType(column_itr) == VALUE_TYPE_VARCHAR
					|| (GetType(column_itr) == VALUE_TYPE_VARBINARY))
					&& !tuple_schema->IsInlined(column_itr)) {
				if (!GetValue(column_itr).IsNull()) {
					bytes += (sizeof(int32_t)
							+ ValuePeeker::PeekObjectLength(
									GetValue(column_itr)));
				}
			}
		}
	}

	return bytes;
}


void Tuple::DeserializeFrom(SerializeInput &input, Pool *dataPool) {
	assert(tuple_schema);
	assert(tuple_data);

	input.ReadInt();
	const int column_count = tuple_schema->GetColumnCount();

	for (int column_itr = 0; column_itr < column_count; column_itr++) {
		const ValueType type = tuple_schema->GetType(column_itr);

		/**
		 * DeserializeFrom is only called when we serialize/deserialize tables.
		 * The serialization format for Strings/Objects in a serialized table
		 * happens to have the same in memory representation as the Strings/Objects
		 * in a Tuple. The goal here is to wrap the serialized representation of
		 * the value in an Value and then serialize that into the tuple from the
		 * Value. This makes it possible to push more value specific functionality
		 * out of Tuple. The memory allocation will be performed when serializing
		 * to tuple storage.
		 */
		const bool is_inlined = tuple_schema->IsInlined(column_itr);
		char *data_ptr = GetDataPtr(column_itr);
		const int32_t column_length = tuple_schema->GetLength(column_itr);

		Value::DeserializeFrom(input, type, data_ptr, is_inlined, column_length, dataPool);
	}
}

int64_t Tuple::DeserializeWithHeaderFrom(SerializeInput &input) {

	int64_t total_bytes_deserialized = 0;

	assert(tuple_schema);
	assert(tuple_data);

	input.ReadInt();  // Read in the tuple size, discard
	total_bytes_deserialized += sizeof(int);

	const int column_count = tuple_schema->GetColumnCount();

	for (int column_itr = 0; column_itr < column_count; column_itr++) {
		const ValueType type = tuple_schema->GetType(column_itr);

		const bool is_inlined = tuple_schema->IsInlined(column_itr);
		char *data_ptr = GetDataPtr(column_itr);
		const int32_t column_length = tuple_schema->GetLength(column_itr);
		total_bytes_deserialized +=
				Value::DeserializeFrom(input, type, data_ptr, is_inlined, column_length, NULL);
	}

	return total_bytes_deserialized;
}

void Tuple::SerializeWithHeaderTo(SerializeOutput &output) {
	assert(tuple_schema);
	assert(tuple_data);

	size_t start = output.Position();
	output.WriteInt(0);  // reserve first 4 bytes for the total tuple size

	const int column_count = tuple_schema->GetColumnCount();

	for (int column_itr = 0; column_itr < column_count; column_itr++) {
		Value value = GetValue(column_itr);
		value.SerializeTo(output);
	}

	int32_t serialized_size = static_cast<int32_t>(output.Position() - start - sizeof(int32_t));

	// write out the length of the tuple at start
	output.WriteIntAt(start, serialized_size);
}

void Tuple::SerializeTo(SerializeOutput &output) {
	size_t start = output.ReserveBytes(4);
	const int column_count = tuple_schema->GetColumnCount();

	for (int column_itr = 0; column_itr < column_count; column_itr++) {
		Value value = GetValue(column_itr);
		value.SerializeTo(output);
	}

	output.WriteIntAt(start, static_cast<int32_t>(output.Position() - start - sizeof(int32_t)));
}

void Tuple::SerializeToExport(ExportSerializeOutput &output, int colOffset, uint8_t *null_array) {
	const int column_count = GetColumnCount();

	for (int column_itr = 0; column_itr < column_count; column_itr++) {
		// NULL doesn't produce any bytes for the Value
		// Handle it here to consolidate manipulation of the nullarray.
		if (IsNull(column_itr)) {
			// turn on relevant bit in nullArray
			int byte = (colOffset + column_itr) >> 3;
			int bit = (colOffset + column_itr) % 8;
			int mask = 0x80 >> bit;
			null_array[byte] = (uint8_t) (null_array[byte] | mask);
			continue;
		}

		GetValue(column_itr).SerializeToExport(output);
	}
}

bool Tuple::operator==(const Tuple &other) const {
	if (tuple_schema != other.tuple_schema) {
		return false;
	}

	return EqualsNoSchemaCheck(other);
}

bool Tuple::operator!=(const Tuple &other) const {
	return !(*this == other);
}

bool Tuple::EqualsNoSchemaCheck(const Tuple &other) const {
	const int column_count = tuple_schema->GetColumnCount();

	for (int column_itr = 0; column_itr < column_count; column_itr++) {
		const Value lhs = GetValue(column_itr);
		const Value rhs = other.GetValue(column_itr);
		if (lhs.OpNotEquals(rhs).IsTrue()) {
			return false;
		}
	}

	return true;
}

void Tuple::SetAllNulls() {
	assert(tuple_schema);
	assert(tuple_data);
	const int column_count = tuple_schema->GetColumnCount();

	for (int column_itr = 0; column_itr < column_count; column_itr++) {
		Value value = Value::GetNullValue(tuple_schema->GetType(column_itr));
		SetValue(column_itr, value);
	}
}

int Tuple::Compare(const Tuple &other) const {
	int diff;
	const int column_count = tuple_schema->GetColumnCount();

	for (int column_itr = 0; column_itr < column_count; column_itr++) {
		const Value lhs = GetValue(column_itr);
		const Value rhs = other.GetValue(column_itr);
		diff = lhs.Compare(rhs);

		if (diff) {
			return diff;
		}
	}

	return 0;
}

// Release to the heap any memory allocated for any uninlined columns.
void Tuple::FreeUninlinedData() {
  if(tuple_data == nullptr)
    return;

	const uint16_t unlinlined_column_count = tuple_schema->GetUninlinedColumnCount();

	for (int column_itr = 0; column_itr < unlinlined_column_count; column_itr++) {
		GetValue(tuple_schema->GetUninlinedColumnIndex(column_itr)).FreeUninlinedData();
	}
}

size_t Tuple::HashCode(size_t seed) const {
	const int column_count = tuple_schema->GetColumnCount();

	for (int column_itr = 0; column_itr < column_count; column_itr++) {
		const Value value = GetValue(column_itr);
		value.HashCombine(seed);
	}

	return seed;
}

size_t Tuple::HashCode() const {
	size_t seed = 0;
	return HashCode(seed);
}

char* Tuple::GetDataPtr(const id_t column_id) {
	assert(tuple_schema);
	assert(tuple_data);
	return &tuple_data[tuple_schema->GetOffset(column_id)];
}

const char* Tuple::GetDataPtr(const id_t column_id) const {
	assert(tuple_schema);
	assert(tuple_data);
	return &tuple_data[tuple_schema->GetOffset(column_id)];
}

// Hasher
struct TupleHasher: std::unary_function<Tuple, std::size_t> {
	// Generate a 64-bit number for the key value
	size_t operator()(Tuple tuple) const {
		return tuple.HashCode();
	}
};

// Equality operator
class TupleEqualityChecker {
public:
	bool operator()(const Tuple lhs, const Tuple rhs) const {
		return lhs.EqualsNoSchemaCheck(rhs);
	}
};

std::ostream& operator<< (std::ostream& os, const Tuple& tuple){

	uint64_t address_num = (uint64_t) tuple.Location();
	os << " @" << address_num << " ";

	id_t column_count = tuple.GetColumnCount();
	for (id_t column_itr = 0; column_itr < column_count; column_itr++) {
		os << "(";
		if (tuple.IsNull(column_itr)) {
			os << "<NULL>";
		}
		else {
			os << tuple.GetValue(column_itr);
		}
		os << ")";
	}

	os << std::endl;

	return os;
}

} // End storage namespace
} // End nstore namespace



