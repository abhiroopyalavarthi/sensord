#pragma once

#include "pipeline/batch.hpp"
#include "pipeline/device_reader.hpp"
#include "telemetry.pb.h"

namespace sensord::server {

void to_proto(const pipeline::Batch& b, uint64_t subscriber_dropped, sensord::v1::SampleBatch* out);
void to_proto(const pipeline::DeviceStatus& s, sensord::v1::DeviceInfo* out);

}  // namespace sensord::server
