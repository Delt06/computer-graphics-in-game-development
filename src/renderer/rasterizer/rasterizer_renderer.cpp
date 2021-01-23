#include "rasterizer_renderer.h"

#include "utils/resource_utils.h"


void cg::renderer::rasterization_renderer::init()
{
	// Load model
	model = std::make_shared<cg::world::model>();
	model->load_obj(settings->model_path);

	// Create render target
	render_target = std::make_shared<cg::resource<cg::unsigned_color>>(settings->width, settings->height);
	camera = std::make_shared<cg::world::camera>();

	// Create depth buffer
	depth_buffer =
		std::make_shared<cg::resource<float>>(settings->width, settings->height);

	camera->set_height(static_cast<float>(settings->height));
	camera->set_width(static_cast<float>(settings->width));

	camera->set_position(float3{ settings->camera_position[0],
								 settings->camera_position[1],
								 settings->camera_position[2] });
	camera->set_theta(settings->camera_theta);
	camera->set_phi(settings->camera_phi);

	camera->set_angle_of_view(settings->camera_angle_of_view);
	camera->set_z_far(settings->camera_z_far);
	camera->set_z_near(settings->camera_z_near);

	// Create rasterizer
	rasterizer = std::make_shared<cg::renderer::rasterizer<vertex, cg::unsigned_color>>();
	rasterizer->set_render_target(render_target, depth_buffer);
	rasterizer->set_vertex_buffer(model->get_vertex_buffer());
	rasterizer->set_viewport(settings->width, settings->height);
}

void cg::renderer::rasterization_renderer::destroy() {}

void cg::renderer::rasterization_renderer::update() {}

inline float smoothstep(float edge0, float edge1, float x)
{
	x = std::clamp((x - edge0) / (edge1 - edge0), 0.f, 1.f);
	return x * x * (3 - 2 * x);
}

void cg::renderer::rasterization_renderer::render()
{
	float4x4 matrix =
		mul(camera->get_projection_matrix(), camera->get_view_matrix(),
			model->get_world_matrix());
	rasterizer->vertex_shader = [&](float4 vertex, cg::vertex vertex_data) {
		auto processed_vertex = mul(matrix, vertex);
		return std::make_pair(processed_vertex, vertex_data);
	};
	rasterizer->pixel_shader = [&](cg::vertex vertex_data, float z) {
		/*return cg::color{ vertex_data.diffuse_r,
						  vertex_data.diffuse_g,
						  vertex_data.diffuse_b};*/
		float3 normal = float3{ vertex_data.nx, vertex_data.ny, vertex_data.nz};
		float3 light_direction = normalize(float3(-0.5f, -1.f, -0.5f));
		float3 towards_light_direction = -light_direction;
		float3 view = -camera->get_direction();
		float3 half = (view + towards_light_direction) * 0.5f;
		float diffuse = dot(normal, towards_light_direction);
		float specular = std::pow(std::clamp(dot(normal, half), 0.f, 1.f), 3.f);
		float fresnel = std::pow(1 - dot(normal, view), 2.5f);

		// clamp
		diffuse = std::clamp(diffuse, 0.f, 1.f);
		specular = std::clamp(specular, 0.f, 1.f);
		fresnel = std::clamp(fresnel, 0.f, 1.f);

		return cg::color{ vertex_data.diffuse_r * diffuse + specular + fresnel + vertex_data.ambient_r * 0.5f,
						  vertex_data.diffuse_g * diffuse + specular + fresnel + vertex_data.ambient_g * 0.5f,
						  vertex_data.diffuse_b * diffuse + specular + fresnel + vertex_data.ambient_b * 0.5f };
	};
	rasterizer->clear_render_target({50, 200, 240});
	rasterizer->draw(model->get_vertex_buffer()->get_number_of_elements(), 0);
	cg::utils::save_resource(*render_target, settings->result_path);
}