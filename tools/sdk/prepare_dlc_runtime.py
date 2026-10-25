"""Generate an opt-in content-manager translation unit from the pinned SDK."""
from pathlib import Path
import sys


def replace(source, old, new):
    if source.count(old) != 1:
        raise ValueError('Content manager anchor changed: ' + old[:100])
    return source.replace(old, new, 1)


def generate(source):
    source = replace(source, '#include <array>', '#include <array>\n#include <set>')
    include = (Path(__file__).resolve().parent / 'dlc_containers.inc').as_posix()
    source = replace(source, 'static int content_device_id_ = 0;',
                     f'static int content_device_id_ = 0;\n#include "{include}"\n'
                     'extern "C" bool Fm4DlcContainersAvailable() { return true; }')
    start = source.index('  auto device = std::make_unique<rex::filesystem::HostPathDevice>')
    end = source.index('  fs->RegisterDevice(std::move(device));', start)
    source = source[:start] + '''  std::unique_ptr<rex::filesystem::Device> device;
  if (std::filesystem::is_regular_file(package_path)) {
    auto original = std::make_unique<OriginalContentDevice>(device_path_, package_path);
    if (!original->Initialize()) return;
    license_ = OriginalContentLicense(original->header(), kernel_state_->user_profile()->xuid());
    device = std::move(original);
  } else {
    device = std::make_unique<rex::filesystem::HostPathDevice>(
        device_path_, package_path, false, /*allow_share_delete=*/true);
    if (!device->Initialize()) return;
  }
''' + source[end:]
    source = replace(source, '''  auto fs = kernel_state_->file_system();
  fs->UnregisterSymbolicLink(root_name_ + ":");''', '''  auto fs = kernel_state_->file_system();
  if (!fs->ResolvePath(device_path_)) return;
  fs->UnregisterSymbolicLink(root_name_ + ":");''')
    source = replace(source, '''  if (!std::filesystem::exists(header_path)) {''', '''  // Container records take precedence over exported metadata.
  if (std::filesystem::is_regular_file(package_path_)) return;
  if (!std::filesystem::exists(header_path)) {''')
    source = replace(source, '''  auto file_infos = rex::filesystem::ListFiles(package_root);
  for (const auto& file_info : file_infos) {''', '''  auto file_infos = rex::filesystem::ListFiles(package_root);
  std::set<std::string> original_publishers;
  std::set<std::pair<std::set<std::string>, uint32_t>> original_grants;
  // Containers take precedence even when an extracted copy has another name.
  for (const auto& info : file_infos) {
    if (info.type != rex::filesystem::FileInfo::Type::kFile) continue;
    const auto path = package_root / info.name;
    auto header = rex::filesystem::StfsContainerDevice::ReadPackageHeader(path);
    if (!header || header->metadata.execution_info.title_id != title_id ||
        header->metadata.content_type != content_type ||
        header->metadata.volume_type != rex::filesystem::XContentVolumeType::kStfs) continue;
    OriginalContentDevice original("", path);
    if (!original.Initialize()) continue;
    auto markers = OriginalPublisherMarkers(original);
    const uint32_t granted = OriginalContentLicense(*header, kernel_state_->user_profile()->xuid());
    if (!markers.empty() && !original_grants.emplace(markers, granted).second) continue;
    XCONTENT_AGGREGATE_DATA data{};
    data.device_id = device_id;
    data.content_type = content_type;
    data.title_id = title_id;
    data.xuid = xuid;
    data.set_file_name(rex::path_to_utf8(info.name));
    data.set_display_name(header->metadata.display_name(rex::system::XLanguage::kEnglish));
    result.emplace_back(std::move(data));
    original_publishers.insert(markers.begin(), markers.end());
  }
  for (const auto& file_info : file_infos) {''')
    source = replace(source, '''    XCONTENT_AGGREGATE_DATA content_data;
    if (XSUCCEEDED(ReadContentHeaderFile''', '''    bool overlaps_original = false;
    for (const auto& marker : original_publishers) {
      if (std::filesystem::exists(package_root / file_info.name / "Media" / marker)) {
        overlaps_original = true;
        break;
      }
    }
    if (overlaps_original) continue;
    XCONTENT_AGGREGATE_DATA content_data{};
    if (XSUCCEEDED(ReadContentHeaderFile''')
    source = replace(source, '''  auto package = std::make_unique<ContentPackage>(kernel_state_, root_name, data, package_path);
  return package;''', '''  if (std::filesystem::is_regular_file(package_path)) {
    auto header = rex::filesystem::StfsContainerDevice::ReadPackageHeader(package_path);
    uint32_t title = data.title_id;
    if (title == kCurrentlyRunningTitleId) title = kernel_state_->title_id();
    if (!header || header->metadata.execution_info.title_id != title ||
        header->metadata.content_type != data.content_type) return nullptr;
  }
  auto package = std::make_unique<ContentPackage>(kernel_state_, root_name, data, package_path);
  if (!kernel_state_->file_system()->ResolvePath(std::string(root_name) + ":")) return nullptr;
  return package;''')
    source = replace(source, '''  assert_not_null(package);
  package->LoadPackageLicenseMask''', '''  if (!package) return X_ERROR_ACCESS_DENIED;
  package->LoadPackageLicenseMask''')
    source = replace(source, '''  std::filesystem::create_directories(package_path);
  if (std::filesystem::exists(package_path))''', '''  if (std::filesystem::is_regular_file(package_path)) return X_ERROR_ACCESS_DENIED;
  std::filesystem::create_directories(package_path);
  if (std::filesystem::exists(package_path))''')
    source = source.replace('''  auto dir_removed = std::filesystem::remove_all(package_path, ec);''', '''  if (std::filesystem::is_regular_file(package_path)) return X_ERROR_ACCESS_DENIED;
  auto dir_removed = std::filesystem::remove_all(package_path, ec);''')
    return source


if __name__ == '__main__':
    result = generate(Path(sys.argv[1]).read_text(encoding='utf-8'))
    output = Path(sys.argv[2])
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_text(encoding='utf-8') != result:
        output.write_text(result, encoding='utf-8')
