#include "raytracer_renderer.h"

#include "utils/resource_utils.h"


void cg::renderer::ray_tracing_renderer::init()
{
	render_target = std::make_shared<cg::resource<cg::unsigned_color>>(
		settings->width, settings->height);

	model = std::make_shared<cg::world::model>();
	model->load_obj(settings->model_path);

	raytracer =
		std::make_shared<cg::renderer::raytracer<cg::vertex, cg::unsigned_color>>();

	raytracer->set_render_target(render_target);
	raytracer->set_viewport(settings->width, settings->height);
	raytracer->set_per_shape_vertex_buffer(model->get_per_shape_buffer());

	camera = std::make_shared<cg::world::camera>();

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

	float3 light_position = float3(0, 1.58f, -0.03f);
	float3 light_color = float3(0.78f, 0.78f, 0.78f);
	lights.push_back({ light_position, light_color });
}

void cg::renderer::ray_tracing_renderer::destroy() {}

void cg::renderer::ray_tracing_renderer::update() {}

void cg::renderer::ray_tracing_renderer::render()
{
	raytracer->clear_render_target({ 0, 0, 0 });
	raytracer->miss_shader = [](const ray& ray) 
	{ 
		payload payload;
		payload.color = { ray.direction.x * 0.5f + 0.5f, ray.direction.y * 0.5f + 0.5f,
						  ray.direction.z * 0.5f + 0.5f};
		return payload;
	};
	raytracer->closest_hit_shader = [&](const ray& ray, payload& payload,
									   const triangle<cg::vertex>& triangle) 
	{
		float3 result_color = triangle.emissive;
		float3 position = ray.position + ray.direction * payload.t;
		float3 normal = payload.bary.x * triangle.na +
						payload.bary.y * triangle.nb + 
						payload.bary.z * triangle.nc;

		for (auto& light : lights)
		{
			cg::renderer::ray to_light(position, light.position - position);
			result_color += triangle.diffuse * light.color *
							std::max(0.f, dot(normal, to_light.direction));
		}

		payload.color = cg::color::from_float3(result_color);
		return payload;
	};

	raytracer->build_acceleration_structure();
	raytracer->ray_generation(
		camera->get_position(), camera->get_direction(), 
		camera->get_right(), camera->get_up());
	cg::utils::save_resource(*render_target, settings->result_path);
}