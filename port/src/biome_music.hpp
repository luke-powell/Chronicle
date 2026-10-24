#pragma once
#include <memory>
#include <string_view>

namespace audio {
class SqFile;
}

void                           BiomeMusicTick(bool interior, bool allowed, double x = 0, double z = 0);
std::shared_ptr<audio::SqFile> BiomeMusicSequence(std::string_view name, std::shared_ptr<audio::SqFile> sequence);
