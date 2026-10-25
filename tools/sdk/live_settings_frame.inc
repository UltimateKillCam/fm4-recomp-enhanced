// Included at the closed-frame boundary, after the existing cache-clear path.
// Runtime graphics settings. GPL-3.0-only.
if (REXCVAR_GET(fm4_live_settings)) {
  fm4::settings::Request change;
  if (fm4::settings::Take(change)) {
    bool success = AwaitAllQueueOperationsCompletion();
    if (success) memory_->CompleteDeferredPhysicalWrites();
    const bool scale_changed = uint32_t(change.scale) != texture_cache_->draw_resolution_scale_x() ||
        uint32_t(change.scale) != texture_cache_->draw_resolution_scale_y();
    const char* temporal = std::getenv("FM4_TEMPORAL_MODE");
    if (temporal && std::string_view(temporal) != "SPATIAL") success = false;
    if (!rex::cvar::Query<bool>("gpu_unlimited_present") &&
        (change.fps == 0 || change.fps > rex::cvar::Query<double>("video_mode_refresh_rate"))) success = false;
    uint32_t scale_x = uint32_t(change.scale), scale_y = uint32_t(change.scale);
    if (!D3D12TextureCache::ClampDrawResolutionScaleToMaxSupported(scale_x, scale_y, provider) ||
        scale_x != uint32_t(change.scale) || scale_y != uint32_t(change.scale)) success = false;
    if (success && scale_changed) {
      // Construct a complete replacement before releasing the working caches.
      // Shader translations contain the scale, and must be replaced too.
      auto new_targets = std::make_unique<D3D12RenderTargetCache>(
          *register_file_, *memory_, trace_writer_, scale_x, scale_y, *this, bindless_resources_used_);
      std::unique_ptr<PipelineCache> new_pipelines;
      std::unique_ptr<D3D12TextureCache> new_textures;
      success = new_targets->Initialize();
      if (success) {
        new_textures = D3D12TextureCache::Create(*register_file_, *shared_memory_,
            scale_x, scale_y, *this, bindless_resources_used_);
        success = bool(new_textures);
      }
      if (success) {
        new_pipelines = std::make_unique<PipelineCache>(*this, *register_file_,
            *new_targets, bindless_resources_used_);
        success = new_pipelines->Initialize();
      }
      if (success) {
        auto reload_shader = [&](Shader* old) -> Shader* {
          if (!old) return nullptr;
          auto words = old->ucode_data();
          // LoadShader consumes guest-endian words, while Shader stores host-endian.
          for (auto& word : words) word = rex::byte_swap(word);
          return new_pipelines->LoadShader(old->type(), words.data(), uint32_t(words.size()));
        };
        auto* vertex = reload_shader(active_vertex_shader_);
        auto* pixel = reload_shader(active_pixel_shader_);
        active_vertex_shader_ = vertex;
        active_pixel_shader_ = pixel;
        // Release dependent consumers before their render-target reference.
        texture_cache_.reset();
        pipeline_cache_.reset();
        render_target_cache_.reset();
        render_target_cache_ = std::move(new_targets);
        pipeline_cache_ = std::move(new_pipelines);
        texture_cache_ = std::move(new_textures);
        if (bindless_resources_used_) {
          render_target_cache_->WriteEdramRawSRVDescriptor(provider.OffsetViewDescriptor(
              view_bindless_heap_cpu_start_, uint32_t(SystemBindlessView::kEdramRawSRV)));
          render_target_cache_->WriteEdramRawUAVDescriptor(provider.OffsetViewDescriptor(
              view_bindless_heap_cpu_start_, uint32_t(SystemBindlessView::kEdramRawUAV)));
          const SystemBindlessView srvs[] = {SystemBindlessView::kEdramR32UintSRV,
              SystemBindlessView::kEdramR32G32UintSRV, SystemBindlessView::kEdramR32G32B32A32UintSRV};
          const SystemBindlessView uavs[] = {SystemBindlessView::kEdramR32UintUAV,
              SystemBindlessView::kEdramR32G32UintUAV, SystemBindlessView::kEdramR32G32B32A32UintUAV};
          for (uint32_t i = 0; i < 3; ++i) {
            render_target_cache_->WriteEdramUintPow2SRVDescriptor(provider.OffsetViewDescriptor(
                view_bindless_heap_cpu_start_, uint32_t(srvs[i])), i + 2);
            render_target_cache_->WriteEdramUintPow2UAVDescriptor(provider.OffsetViewDescriptor(
                view_bindless_heap_cpu_start_, uint32_t(uavs[i])), i + 2);
          }
        }
        ui::d3d12::util::ReleaseAndNull(scratch_buffer_);
        scratch_buffer_size_ = 0;
        resolve_downscale_buffer_.Reset();
        resolve_downscale_buffer_size_ = 0;
        fxaa_source_texture_.Reset();
        fxaa_source_texture_submission_ = 0;
        // No old cached descriptor layouts or pipeline handles may cross this boundary.
        current_texture_layout_uid_vertex_ = current_texture_layout_uid_pixel_ = SIZE_MAX;
        current_sampler_layout_uid_vertex_ = current_sampler_layout_uid_pixel_ = SIZE_MAX;
        current_samplers_vertex_.clear(); current_samplers_pixel_.clear();
        current_sampler_bindless_indices_vertex_.clear(); current_sampler_bindless_indices_pixel_.clear();
        current_guest_pipeline_ = nullptr;
        current_external_pipeline_ = nullptr;
        REXCVAR_SET(resolution_scale, change.scale);
      }
    }
    if (success) {
      static const char* const effects[] = {"bilinear", "fsr", "cas"};
      static const char* const posts[] = {"none", "fxaa", "fxaa_extreme"};
      rex::cvar::SetFlagByName("present_effect", effects[change.effect]);
      rex::cvar::SetFlagByName("swap_post_effect", posts[change.post]);
      swap_post_effect_actual_ = swap_post_effect_desired_ = SwapPostEffect(change.post);
      rex::cvar::SetFlagByName("texture_mip_bias", std::to_string(change.mip_bias));
      rex::cvar::SetFlagByName("anisotropic_override", std::to_string(change.anisotropy));
      // Sampler parameters are cached without mip bias in the key. Reuse the
      // established drained cache clear to invalidate both bindless and bindful descriptors.
      if (bindless_resources_used_) {
        texture_cache_bindless_sampler_map_.clear();
        for (const auto& heap : sampler_bindless_heaps_overflowed_) heap.first->Release();
        sampler_bindless_heaps_overflowed_.clear();
        sampler_bindless_heap_allocated_ = 0;
      } else {
        sampler_bindful_heap_pool_->ClearCache();
      }
      current_sampler_layout_uid_vertex_ = current_sampler_layout_uid_pixel_ = SIZE_MAX;
      current_samplers_vertex_.clear(); current_samplers_pixel_.clear();
      current_sampler_bindless_indices_vertex_.clear(); current_sampler_bindless_indices_pixel_.clear();
      OnRegistersRestored();
    }
    fm4::settings::Complete(change, success);
  }
  fm4::settings::Pace();
}
