#include "dx12_renderer.h"

#include "utils/com_error_handler.h"
#include "utils/window.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <filesystem>


void cg::renderer::dx12_renderer::init()
{
	// Default values
	rtv_descriptor_size = 0;
	view_port = CD3DX12_VIEWPORT(
		0.f, 0.f, static_cast<float>(settings->width), 
		static_cast<float>(settings->height));
	scissor_rect = CD3DX12_RECT(
		0, 0, static_cast<LONG>(settings->width),
		static_cast<LONG>(settings->height));
	vertex_buffer_view = {};

	constant_buffer_data_begin = nullptr;
	frame_index = 0;

	for (size_t i = 0; i < frame_number; i++)
	{
		fence_values[i] = 0;
	}

	// Load model
	model = std::make_shared<cg::world::model>();
	model->load_obj(settings->model_path);

	// Prepare camera
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

	world_view_projection =
		camera->get_dxm_view_matrix() * camera->get_dxm_projection_matrix();

	load_pipeline();
	load_assets();
}

void cg::renderer::dx12_renderer::destroy()
{
	wait_for_gpu();
	CloseHandle(fence_event);
}

void cg::renderer::dx12_renderer::update()
{
	world_view_projection =
		camera->get_dxm_view_matrix() * camera->get_dxm_projection_matrix();
	memcpy(constant_buffer_data_begin, &world_view_projection, sizeof(world_view_projection));
}

void cg::renderer::dx12_renderer::render()
{
	populate_command_list();

	ID3D12CommandList* command_lists[] = { command_list.Get() };
	command_queue->ExecuteCommandLists(_countof(command_lists), command_lists);

	THROW_IF_FAILED(swap_chain->Present(0, 0));

	move_to_next_frame();
}

void cg::renderer::dx12_renderer::load_pipeline()
{
	// Debug layer
	UINT dxgi_factory_flags = 0;
#ifdef DEBUG
	ComPtr<ID3D12Debug> debug_controller;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_controller)))) 
	{
		debug_controller->EnableDebugLayer();
		dxgi_factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
	}
#endif // DEBUG

	// Hardware adapter
	ComPtr<IDXGIFactory4> dxgi_factory;
	THROW_IF_FAILED(CreateDXGIFactory2(dxgi_factory_flags, IID_PPV_ARGS(&dxgi_factory)));

	ComPtr<IDXGIAdapter1> hardware_adapter;
	dxgi_factory->EnumAdapters1(0, &hardware_adapter);
#ifdef DEBUG
	DXGI_ADAPTER_DESC adapter_desc = {};
	hardware_adapter->GetDesc(&adapter_desc);
	OutputDebugString(adapter_desc.Description);
#endif // DEBUG

	// Create device
	THROW_IF_FAILED(
		D3D12CreateDevice(hardware_adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));

	// Create command queue
	D3D12_COMMAND_QUEUE_DESC queue_descriptor = {};
	queue_descriptor.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queue_descriptor.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	THROW_IF_FAILED(
		device->CreateCommandQueue(&queue_descriptor, IID_PPV_ARGS(&command_queue)));

	// Create swap chain
	DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {};
	swap_chain_desc.BufferCount = frame_number;
	swap_chain_desc.Width = settings->width;
	swap_chain_desc.Height = settings->height;
	swap_chain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swap_chain_desc.SampleDesc.Count = 1;

	ComPtr<IDXGISwapChain1> temp_swap_chain;
	THROW_IF_FAILED(dxgi_factory->CreateSwapChainForHwnd(
		command_queue.Get(), cg::utils::window::get_hwnd(), 
		&swap_chain_desc,
		nullptr, nullptr, &temp_swap_chain));

	THROW_IF_FAILED(dxgi_factory->MakeWindowAssociation(
		cg::utils::window::get_hwnd(), DXGI_MWA_NO_ALT_ENTER));
	THROW_IF_FAILED(temp_swap_chain.As(&swap_chain));

	frame_index = swap_chain->GetCurrentBackBufferIndex();

	// Create descriptor heap for RT
	D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
	rtv_heap_desc.NumDescriptors = frame_number;
	rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	THROW_IF_FAILED(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap)));
	rtv_descriptor_size =
		device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	// Create render target view for RTs
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtv_handle(rtv_heap->GetCPUDescriptorHandleForHeapStart());
	for (UINT i = 0; i < frame_number; i++)
	{
		THROW_IF_FAILED(swap_chain->GetBuffer(i, IID_PPV_ARGS(&render_targets[i])));
		device->CreateRenderTargetView(render_targets[i].Get(), nullptr, rtv_handle);
		rtv_handle.Offset(1, rtv_descriptor_size);
		std::wstring name = L"Render target ";
		name += std::to_wstring(i);
		render_targets[i]->SetName(name.c_str());
	}

	// Create depth stencil descriptor heap 
	D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc = {};
	dsv_heap_desc.NumDescriptors = 1;
	dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	THROW_IF_FAILED(
		device->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(&dsv_heap)));

	// Create depth buffer
	CD3DX12_RESOURCE_DESC depth_buffer_desc(
		D3D12_RESOURCE_DIMENSION_TEXTURE2D,
		0, settings->width, settings->height, 1, 1, 
		DXGI_FORMAT_D32_FLOAT,
		1, 0, 
		D3D12_TEXTURE_LAYOUT_UNKNOWN,
		D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE
	);
	D3D12_CLEAR_VALUE depth_clear_value{};
	depth_clear_value.Format = DXGI_FORMAT_D32_FLOAT;
	depth_clear_value.DepthStencil.Depth = 1.0f;
	depth_clear_value.DepthStencil.Stencil = 0;

	THROW_IF_FAILED(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), 
		D3D12_HEAP_FLAG_NONE,
		&depth_buffer_desc,
		D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear_value, IID_PPV_ARGS(&depth_buffer)));
	depth_buffer->SetName(L"Depth buffer");

	device->CreateDepthStencilView(depth_buffer.Get(), nullptr, dsv_heap->GetCPUDescriptorHandleForHeapStart());


	// Create descriptor heap for CBV
	D3D12_DESCRIPTOR_HEAP_DESC cbv_heap_desc = {};
	cbv_heap_desc.NumDescriptors = 1 + model->get_per_shape_texture_files().size();
	cbv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	cbv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	THROW_IF_FAILED(
		device->CreateDescriptorHeap(&cbv_heap_desc, IID_PPV_ARGS(&cbv_srv_heap)));

	for (size_t i = 0; i < frame_number; i++)
	{
		THROW_IF_FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&command_allocators[i])));
	}
}

void cg::renderer::dx12_renderer::load_assets()
{
	// Create a root signature
	D3D12_FEATURE_DATA_ROOT_SIGNATURE rs_feature_data = {};
	rs_feature_data.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;

	if (FAILED(device->CheckFeatureSupport(
			D3D12_FEATURE_ROOT_SIGNATURE, &rs_feature_data, sizeof(rs_feature_data))))
	{
		rs_feature_data.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
	}

	// Create descriptor tables
	CD3DX12_DESCRIPTOR_RANGE1 ranges[2];
	CD3DX12_ROOT_PARAMETER1 root_parameters[2];
	ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
	ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
	root_parameters[0].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_VERTEX);
	root_parameters[1].InitAsDescriptorTable(1, &ranges[1], D3D12_SHADER_VISIBILITY_PIXEL);

	D3D12_ROOT_SIGNATURE_FLAGS rs_flags =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	D3D12_STATIC_SAMPLER_DESC sampler_desc{};
	sampler_desc.Filter = D3D12_FILTER_ANISOTROPIC;
	sampler_desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler_desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler_desc.MipLODBias = 0;
	sampler_desc.MaxAnisotropy = 16;
	sampler_desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	sampler_desc.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
	sampler_desc.MinLOD = 0;
	sampler_desc.MaxLOD = D3D12_FLOAT32_MAX;
	sampler_desc.ShaderRegister = 0;
	sampler_desc.RegisterSpace = 0;
	sampler_desc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rs_descriptor;
	rs_descriptor.Init_1_1(_countof(root_parameters), root_parameters, 1, &sampler_desc, rs_flags);

	ComPtr<ID3DBlob> signature;
	ComPtr<ID3DBlob> error;

	HRESULT result = D3DX12SerializeVersionedRootSignature(&rs_descriptor, rs_feature_data.HighestVersion, &signature, &error);
	if (FAILED(result))
	{
		OutputDebugStringA((char*)error->GetBufferPointer());
		THROW_IF_FAILED(result);
	}

	THROW_IF_FAILED(device->CreateRootSignature(
		0, signature->GetBufferPointer(), signature->GetBufferSize(),
		IID_PPV_ARGS(&root_signature)));

	// Compile shaders 
	WCHAR buffer[MAX_PATH];
	GetModuleFileName(NULL, buffer, MAX_PATH);
	auto shader_path = std::filesystem::path(buffer).parent_path() / "shaders.hlsl";
	ComPtr<ID3DBlob> vertex_shader;
	ComPtr<ID3DBlob> pixel_shader;
	ComPtr<ID3DBlob> pixel_shader_texture;

	UINT compile_flags = 0;
#ifdef DEBUG
	compile_flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif // DEBUG
	result = D3DCompileFromFile(shader_path.wstring().c_str(), nullptr, nullptr,
								"VSMain", "vs_5_0", compile_flags, 0, &vertex_shader, &error);
	if (FAILED(result))
	{
		OutputDebugStringA((char*)error->GetBufferPointer());
		THROW_IF_FAILED(result);
	}

	result = D3DCompileFromFile(
		shader_path.wstring().c_str(), nullptr, nullptr, "PSMain", "ps_5_0",
		compile_flags, 0, &pixel_shader, &error);
	if (FAILED(result))
	{
		OutputDebugStringA((char*)error->GetBufferPointer());
		THROW_IF_FAILED(result);
	}

	result = D3DCompileFromFile(
		shader_path.wstring().c_str(), nullptr, nullptr, "PSMain_texture", "ps_5_0",
		compile_flags, 0, &pixel_shader_texture, &error);
	if (FAILED(result))
	{
		OutputDebugStringA((char*)error->GetBufferPointer());
		THROW_IF_FAILED(result);
	}

	D3D12_INPUT_ELEMENT_DESC input_element_descriptors[] = { 
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, 
			D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 36,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 2, DXGI_FORMAT_R32G32B32_FLOAT, 0, 48,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 60,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_descriptor = {};
	pso_descriptor.InputLayout = { input_element_descriptors,
								   _countof(input_element_descriptors) };
	pso_descriptor.pRootSignature = root_signature.Get();
	pso_descriptor.VS = CD3DX12_SHADER_BYTECODE(vertex_shader.Get());
	pso_descriptor.PS = CD3DX12_SHADER_BYTECODE(pixel_shader.Get());
	pso_descriptor.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	pso_descriptor.RasterizerState.FrontCounterClockwise = TRUE;
	pso_descriptor.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pso_descriptor.RasterizerState.DepthClipEnable = FALSE;
	pso_descriptor.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	pso_descriptor.DepthStencilState.DepthEnable = TRUE;
	pso_descriptor.DepthStencilState.StencilEnable = FALSE;
	pso_descriptor.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	pso_descriptor.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	pso_descriptor.SampleMask = UINT_MAX;
	pso_descriptor.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pso_descriptor.NumRenderTargets = 1;
	pso_descriptor.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	pso_descriptor.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	pso_descriptor.SampleDesc.Count = 1;

	THROW_IF_FAILED(device->CreateGraphicsPipelineState(&pso_descriptor, IID_PPV_ARGS(&pipeline_state)));

	pso_descriptor.PS = CD3DX12_SHADER_BYTECODE(pixel_shader_texture.Get());

	THROW_IF_FAILED(device->CreateGraphicsPipelineState(
		&pso_descriptor, IID_PPV_ARGS(&pipeline_state_textures)));

	// Create command list
	THROW_IF_FAILED(device->CreateCommandList(
		0, D3D12_COMMAND_LIST_TYPE_DIRECT, command_allocators[0].Get(),
		pipeline_state.Get(), IID_PPV_ARGS(&command_list)));

	

	upload_vertex_buffer.resize(model->get_per_shape_index_buffer().size());
	vertex_buffer.resize(model->get_per_shape_index_buffer().size());
	vertex_buffer_view.resize(model->get_per_shape_index_buffer().size());

	upload_index_buffer.resize(model->get_per_shape_index_buffer().size());
	index_buffer.resize(model->get_per_shape_index_buffer().size());
	index_buffer_view.resize(model->get_per_shape_index_buffer().size());

	upload_textures.resize(model->get_per_shape_index_buffer().size());
	textures.resize(model->get_per_shape_index_buffer().size());

	const UINT cbv_srv_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	CD3DX12_CPU_DESCRIPTOR_HANDLE cbv_srv_handle(cbv_srv_heap->GetCPUDescriptorHandleForHeapStart(), 1, cbv_srv_descriptor_size);

	for (size_t s = 0; s < model->get_per_shape_buffer().size(); s++)
	{
		// Create and upload vertex buffer
		auto vertex_buffer_data = model->get_per_shape_buffer()[s];
		const UINT vertex_buffer_size =
			static_cast<UINT>(vertex_buffer_data->get_size_in_bytes());

		THROW_IF_FAILED(device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(vertex_buffer_size),
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
			IID_PPV_ARGS(&upload_vertex_buffer[s])));

		THROW_IF_FAILED(device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(vertex_buffer_size),
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&vertex_buffer[s])));
		std::wstring name(L"Vertex buffer ");
		name += std::to_wstring(s);
		vertex_buffer[s]->SetName(name.c_str());

		D3D12_SUBRESOURCE_DATA vertex_data{};
		vertex_data.pData = vertex_buffer_data->get_data();
		vertex_data.RowPitch = vertex_buffer_size;
		vertex_data.SlicePitch = vertex_buffer_size;
		UpdateSubresources(
			command_list.Get(), vertex_buffer[s].Get(), upload_vertex_buffer[s].Get(),
			0, 0, 1, &vertex_data);
		command_list->ResourceBarrier(
			1, &CD3DX12_RESOURCE_BARRIER::Transition(
				   vertex_buffer[s].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
				   D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));

		// Create VB descriptor
		vertex_buffer_view[s].BufferLocation =
			vertex_buffer[s]->GetGPUVirtualAddress();
		vertex_buffer_view[s].SizeInBytes = vertex_buffer_size;
		vertex_buffer_view[s].StrideInBytes = sizeof(cg::vertex);

		// Create and upload index buffer
		auto index_buffer_data = model->get_per_shape_index_buffer()[s];
		const UINT index_buffer_size =
			static_cast<UINT>(index_buffer_data->get_size_in_bytes());

		THROW_IF_FAILED(device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(index_buffer_size),
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
			IID_PPV_ARGS(&upload_index_buffer[s])));

		THROW_IF_FAILED(device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(index_buffer_size),
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&index_buffer[s])));

		std::wstring index_name(L"Index buffer ");
		index_name += std::to_wstring(s);
		index_buffer[s]->SetName(index_name.c_str());

		D3D12_SUBRESOURCE_DATA index_data{};
		index_data.pData = index_buffer_data->get_data();
		index_data.RowPitch = index_buffer_size;
		index_data.SlicePitch = index_buffer_size;
		UpdateSubresources(
			command_list.Get(), index_buffer[s].Get(),
			upload_index_buffer[s].Get(),
			0, 0, 1, &index_data);
		command_list->ResourceBarrier(
			1, &CD3DX12_RESOURCE_BARRIER::Transition(
				   index_buffer[s].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
				   D3D12_RESOURCE_STATE_INDEX_BUFFER));

		// Create IB descriptor
		index_buffer_view[s].BufferLocation = index_buffer[s]->GetGPUVirtualAddress();
		index_buffer_view[s].SizeInBytes = index_buffer_size;
		index_buffer_view[s].Format = DXGI_FORMAT_R32_UINT;

		// Upload texture
		if (model->get_per_shape_texture_files()[s].empty())
		{
			cbv_srv_handle.Offset(1, cbv_srv_descriptor_size);
			continue;
		}

		std::string full_name =
			std::filesystem::absolute(model->get_per_shape_texture_files()[s]).string();

		int tex_width, tex_height, tex_channels;
		unsigned char* image =
			stbi_load(full_name.c_str(), &tex_width, &tex_height, &tex_channels, STBI_rgb_alpha);

		if (image == nullptr)
		{
			throw std::runtime_error("Can't load texture.");
		}

		D3D12_RESOURCE_DESC texture_desc{};
		texture_desc.MipLevels = 1;
		texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		texture_desc.Width = tex_width;
		texture_desc.Height = tex_height;
		texture_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
		texture_desc.DepthOrArraySize = 1;
		texture_desc.SampleDesc.Count = 1;
		texture_desc.SampleDesc.Quality = 0;
		texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

		THROW_IF_FAILED(device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE, &texture_desc,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&textures[s])));

		const UINT64 upload_buffer_size = GetRequiredIntermediateSize(textures[s].Get(), 0, 1);

		THROW_IF_FAILED(device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(upload_buffer_size),
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
			IID_PPV_ARGS(&upload_textures[s])));

		D3D12_SUBRESOURCE_DATA texture_data{};
		texture_data.pData = image;
		texture_data.RowPitch = tex_width * STBI_rgb_alpha;
		texture_data.SlicePitch = texture_data.RowPitch * tex_height;

		UpdateSubresources(
			command_list.Get(), textures[s].Get(), upload_textures[s].Get(), 0,
			0, 1, &texture_data);
		command_list->ResourceBarrier(
			1, &CD3DX12_RESOURCE_BARRIER::Transition(
				   textures[s].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
				   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

		D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
		srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv_desc.Format = texture_desc.Format;
		srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv_desc.Texture2D.MipLevels = 1;

		device->CreateShaderResourceView(textures[s].Get(), &srv_desc, cbv_srv_handle);
		cbv_srv_handle.Offset(1, cbv_srv_descriptor_size);
	}

	// Create and upload constant buffer
	THROW_IF_FAILED(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(64*1024),
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&constant_buffer)));

	CD3DX12_RANGE read_range(0, 0);

	THROW_IF_FAILED(
		constant_buffer->Map(0, &read_range, reinterpret_cast<void**>(&constant_buffer_data_begin)));
	constant_buffer->SetName(L"Constant buffer");
	memcpy(constant_buffer_data_begin, &world_view_projection, sizeof(world_view_projection));

	// Create CBV descriptor
	D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_descriptor = {};
	cbv_descriptor.BufferLocation = constant_buffer->GetGPUVirtualAddress();
	cbv_descriptor.SizeInBytes = (sizeof(world_view_projection) + 255) & ~255;

	device->CreateConstantBufferView(
		&cbv_descriptor, cbv_srv_heap->GetCPUDescriptorHandleForHeapStart());

	THROW_IF_FAILED(command_list->Close());
	ID3D12CommandList* command_lists[] = { command_list.Get() };
	command_queue->ExecuteCommandLists(_countof(command_lists), command_lists);

	// Create sync
	THROW_IF_FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
	fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (fence_event == nullptr)
	{
		THROW_IF_FAILED(HRESULT_FROM_WIN32(GetLastError()));
	}

	wait_for_gpu();
}

void cg::renderer::dx12_renderer::populate_command_list()
{
	// Resetting
	THROW_IF_FAILED(command_allocators[frame_index]->Reset());
	THROW_IF_FAILED(command_list->Reset(command_allocators[frame_index].Get(), pipeline_state.Get()));

	// Initial state
	command_list->SetGraphicsRootSignature(root_signature.Get());
	ID3D12DescriptorHeap* heaps[] = { cbv_srv_heap.Get() };
	command_list->SetDescriptorHeaps(_countof(heaps), heaps);
	command_list->SetGraphicsRootDescriptorTable(0, cbv_srv_heap->GetGPUDescriptorHandleForHeapStart());
	command_list->RSSetViewports(1, &view_port);
	command_list->RSSetScissorRects(1, &scissor_rect);

	command_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
		render_targets[frame_index].Get(),
		D3D12_RESOURCE_STATE_PRESENT,
		D3D12_RESOURCE_STATE_RENDER_TARGET
	));

	// Drawing
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtv_handle(
		rtv_heap->GetCPUDescriptorHandleForHeapStart(), frame_index, rtv_descriptor_size);
	CD3DX12_CPU_DESCRIPTOR_HANDLE dsv_handle(
		dsv_heap->GetCPUDescriptorHandleForHeapStart());
	command_list->OMSetRenderTargets(1, &rtv_handle, TRUE, &dsv_handle);
	const float clear_color[] = { 0.f, 0.f, 0.f, 1.f };
	command_list->ClearRenderTargetView(rtv_handle, clear_color, 0, nullptr);
	command_list->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
	command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	const UINT cbv_srv_descriptor_size = device->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	CD3DX12_GPU_DESCRIPTOR_HANDLE cbv_srv_handle(
		cbv_srv_heap->GetGPUDescriptorHandleForHeapStart(), 1, cbv_srv_descriptor_size);

	bool is_plain_color = true;


	for (size_t s = 0; s < model->get_per_shape_buffer().size(); s++)
	{
		if (!model->get_per_shape_texture_files()[s].empty())
		{
			cbv_srv_handle.InitOffsetted(cbv_srv_heap->GetGPUDescriptorHandleForHeapStart(), s+1, cbv_srv_descriptor_size);
			command_list->SetGraphicsRootDescriptorTable(1, cbv_srv_handle);

			if (is_plain_color)
				command_list->SetPipelineState(pipeline_state_textures.Get());

			is_plain_color = false;
		}
		else if (!is_plain_color)
		{
			command_list->SetPipelineState(pipeline_state.Get());
			is_plain_color = true;
		}

		command_list->IASetVertexBuffers(0, 1, &vertex_buffer_view[s]);
		command_list->IASetIndexBuffer(&index_buffer_view[s]);
		command_list->DrawIndexedInstanced(
			model->get_per_shape_index_buffer()[s]->get_number_of_elements(), 1, 0, 0, 0);
	}

	command_list->ResourceBarrier(
		1, &CD3DX12_RESOURCE_BARRIER::Transition(
			   render_targets[frame_index].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
			   D3D12_RESOURCE_STATE_PRESENT));
	THROW_IF_FAILED(command_list->Close());
}


void cg::renderer::dx12_renderer::move_to_next_frame()
{
	const UINT64 current_fence_value = fence_values[frame_index];
	THROW_IF_FAILED(command_queue->Signal(fence.Get(), current_fence_value));

	frame_index = swap_chain->GetCurrentBackBufferIndex();

	if (fence->GetCompletedValue() < fence_values[frame_index])
	{
		THROW_IF_FAILED(
			fence->SetEventOnCompletion(fence_values[frame_index], fence_event));
		WaitForSingleObjectEx(fence_event, INFINITE, FALSE);
	}

	fence_values[frame_index] = current_fence_value + 1;
}

void cg::renderer::dx12_renderer::wait_for_gpu()
{
	THROW_IF_FAILED(command_queue->Signal(fence.Get(), fence_values[frame_index]));

	THROW_IF_FAILED(fence->SetEventOnCompletion(fence_values[frame_index], fence_event));

	WaitForSingleObjectEx(fence_event, INFINITE, FALSE);

	fence_values[frame_index]++;
}
