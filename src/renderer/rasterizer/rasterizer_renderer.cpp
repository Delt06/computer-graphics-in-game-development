#include "rasterizer_renderer.h"

#include "utils/resource_utils.h"


void cg::renderer::rasterization_renderer::init()
{
	// Create render target
	render_target = std::make_shared<cg::resource<cg::unsigned_color>>(settings->width, settings->height);

	// Create rasterizer
	rasterizer = std::make_shared<cg::renderer::rasterizer<vertex, cg::unsigned_color>>();
	rasterizer->set_render_target(render_target);
}

void cg::renderer::rasterization_renderer::destroy() {}

void cg::renderer::rasterization_renderer::update() {}

void cg::renderer::rasterization_renderer::render()
{
	rasterizer->clear_render_target({255, 255, 0});
	cg::utils::save_resource(*render_target, settings->result_path);
}