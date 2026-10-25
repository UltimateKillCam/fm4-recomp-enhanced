// Original containers are mounted without extraction or writable host access.
class OriginalContentDevice final : public rex::filesystem::StfsContainerDevice {
 public:
  using StfsContainerDevice::StfsContainerDevice;
  bool is_read_only() const override { return true; }
};

static uint32_t OriginalContentLicense(const rex::filesystem::StfsHeader& header,
                                       uint64_t profile) {
  uint32_t result = 0;
  for (const auto& entry : header.header.licenses) {
    const uint64_t identity = entry.licensee_id;
    // Console-specific identities need a matching console identity; accept
    // universal records and this signed-in local profile.
    if ((entry.license_flags & 1) &&
        (identity == UINT64_MAX || (profile && identity == profile))) {
      result |= entry.license_bits;
    }
  }
  return result;
}

static std::set<std::string> OriginalPublisherMarkers(rex::filesystem::Device& device) {
  std::set<std::string> result;
  auto media = device.ResolvePath("Media");
  if (media) {
    for (const auto& child : media->children()) {
      auto name = child->name();
      if (name.ends_with(".puboffer")) result.insert(name);
    }
  }
  return result;
}
