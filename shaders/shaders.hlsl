cbuffer ConstantBuffer : register(b0)
{
	float4x4 mwpMatrix;
}

struct PSInput
{
	float4 position : SV_POSITION;
	float4 color : COLOR;
	float4 normal : NORMAL;
	float4 light_position : TEXCOORD0;
};

PSInput VSMain(float4 position
			   : POSITION, float4 normal
			   : NORMAL, float4 ambient
			   : COLOR0, float4 diffuse
			   : COLOR1, float4 emissive
			   : COLOR2)
{
	PSInput result;

	result.position = mul(mwpMatrix, position);
	result.color = ambient;
	result.normal = mul(mwpMatrix, normal);
	result.light_position = mul(mwpMatrix, float4(0, 2, 0, 0));

	return result;
}

float4 PSMain(PSInput input) : SV_TARGET
{
	float4 color = input.color;
	float4 light_direction = normalize(input.light_position - input.position);
	color *= saturate(dot(normalize(input.normal), light_direction) + 0.5);


	float distance = length(input.position / 100 + float4(100, 100, 0, 0));
	color += float4(0, 0.2, 0, 0) * smoothstep(-2, 0.5, distance - round(distance));
	distance =
		length(input.position * float4(0, -1, 0, 0) / 100 + float4(100, 100, 0, 0));
	color += float4(0.2, 0, 0, 0) * smoothstep(-2, 0.5, distance - round(distance));
	distance = length(
		input.position * float4(-1, 0.5, 0, 0) / 100 + float4(100, 100, 0, 0));
	color += float4(0, 0, 0.2, 0) * smoothstep(-2, 0.5, distance - round(distance));


	return color;
}
