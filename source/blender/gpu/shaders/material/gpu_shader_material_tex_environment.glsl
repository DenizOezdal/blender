void node_tex_environment_texco(vec3 viewvec, out vec3 worldvec)
{
#ifdef MESH_SHADER
  worldvec = worldPosition;
#else
  vec4 v = (ProjectionMatrix[3][3] == 0.0) ? vec4(viewvec, 1.0) : vec4(0.0, 0.0, 1.0, 1.0);
  vec4 co_homogeneous = (ProjectionMatrixInverse * v);

  vec3 co = co_homogeneous.xyz / co_homogeneous.w;
#  if defined(WORLD_BACKGROUND) || defined(PROBE_CAPTURE)
  worldvec = mat3(ViewMatrixInverse) * co;
#  else
  worldvec = mat3(ModelMatrixInverse) * (mat3(ViewMatrixInverse) * co);
#  endif
#endif
}

void node_tex_environment_equirectangular(vec3 co, out vec3 uv)
{
  vec3 nco = normalize(co);
  uv.x = -atan(nco.y, nco.x) / (2.0 * M_PI) + 0.5;
  uv.y = atan(nco.z, hypot(nco.x, nco.y)) / M_PI + 0.5;
}

void node_tex_environment_mirror_ball(vec3 co, out vec3 uv)
{
  vec3 nco = normalize(co);
  nco.y -= 1.0;

  float div = 2.0 * sqrt(max(-0.5 * nco.y, 0.0));
  nco /= max(1e-8, div);

  uv = 0.5 * nco.xzz + 0.5;
}

int node_tex_environment_cubemap_projection(vec3 co, out float u, out float v, out float maxAxis)
{
 // TODO credit copy & paste from wikipedia
  float absX = abs(co.x);
  float absY = abs(co.y);
  float absZ = abs(co.z);
  
  bool isXPositive = co.x > 0 ? true : false;
  bool isYPositive = co.y > 0 ? true : false;
  bool isZPositive = co.z > 0 ? true : false;

  int index = -1;

   // POSITIVE X
  if (isXPositive && absX >= absY && absX >= absZ)
  {
    maxAxis = absX;
    u = co.y;
    v = co.z;
    index = 0;
  }
  // NEGATIVE X
  if (!isXPositive && absX >= absY && absX >= absZ)
  {
    maxAxis = absX;
    u = -co.y;
    v = co.z;
    index = 1;
  }
  // POSITIVE Y
  if (isYPositive && absY >= absX && absY >= absZ)
  {
    maxAxis = absY;
    u = -co.x;
    v = co.z;
    index = 2;
  }
  // NEGATIVE Y
  if (!isYPositive && absY >= absX && absY >= absZ)
  {
    maxAxis = absY;
    u = co.x;
    v = co.z;
    index = 3;
  }
  // POSITIVE Z
  if (isZPositive && absZ >= absX && absZ >= absY)
  {
    maxAxis = absZ;
    u = co.x;
    v = co.y;
    index = 4;
  }
  // NEGATIVE Z
  if (!isZPositive && absZ >= absX && absZ >= absY)
  {
    maxAxis = absZ;
    u = co.x;
    v = co.y;
    index = 5;
  }

  return index;
}

void node_tex_environment_cubemap_cross_horizontal(vec3 co, out vec3 uv)
{
  float maxAxis = 0.0;
  float uc = 0.0;
  float vc = 0.0;
  int index = node_tex_environment_cubemap_projection(co, uc, vc, maxAxis);
  // Convert u range from -1 to 1 to 0 to 0.25 (1/4) as the texture space is 4 faces wide
  float u = 0.125f * (uc / maxAxis + 1.0f);
  // Convert v range from -1 to 1 to 0 to 0.333.. (1/3) as the texture space is 3 faces high
  float v = 0.166667f * (vc / maxAxis + 1.0f);

  // Some coordinates are reordered to take Blender's internal transform system into account
  // while we offset the uv coordinate to the right face.
  switch(index) {
    case 0:
    {
      u += 0.5;
      v += 0.333333f;
      break;
    }
    case 1:
    {
      v += 0.333333f;
      break;
    }
    case 2:
    {
      // -Z
      u += 0.75;
      v += 0.333333f;
      break;
    }
    case 3:
    {
      // Z
      u += 0.25;
      v += 0.333333f;
      break;
    }
    case 4:
    {
      // Y
      u += 0.25;
      v += 0.666666f;
      break;
    }
    case 5:
    {
      // -Y
      u += 0.25;
      v = 0.333333f - v;
      break;
    }
  }

  uv.x = u;
  uv.y = v;
}

void node_tex_environment_cubemap_stripe_horizontal(vec3 co, out vec3 uv)
{
  float maxAxis = 0.0;
  float uc = 0.0;
  float vc = 0.0;
  int index = node_tex_environment_cubemap_projection(co, uc, vc, maxAxis);
  // Convert u range from -1 to 1 to 0 to 0.166667 (1/6) as the texture space is 6 faces wide
  float u = 0.083333f * (uc / maxAxis + 1.0f);
  // Convert v range from -1 to 1 to 0 to 1 as the texture space is 1 face high
  float v = 0.5f * (vc / maxAxis + 1.0f);

  // Some coordinates are reordered to take Blender's internal transform system into account
  // while we offset the uv coordinate to the right face.
  switch(index) {
    case 1:
    {
      u += 0.166666;
      break;
    }
    case 2:
    {
      // -Z
      u += 0.166666 * 5;
      break;
    }
    case 3:
    {
      // Z
      u += 0.166666 * 4;
      break;
    }
    case 4:
    {
      // Y
      u += 0.166666 * 2;
      break;
    }
    case 5:
    {
      // -Y
      u += 0.166666 * 3;
      break;
    }
  }

  uv.x = u;
  uv.y = v;
}

void node_tex_environment_cubemap_stripe_vertical(vec3 co, out vec3 uv)
{
  float maxAxis = 0.0;
  float uc = 0.0;
  float vc = 0.0;
  int index = node_tex_environment_cubemap_projection(co, uc, vc, maxAxis);
  // Convert v range from -1 to 1 to 0 to 1 as the texture space is 1 face high
  float u = 0.5f * (uc / maxAxis + 1.0f);
  // Convert u range from -1 to 1 to 0 to 0.166667 (1/6) as the texture space is 6 faces high
  float v = 0.083333f * (vc / maxAxis + 1.0f);

  // Some coordinates are reordered to take Blender's internal transform system into account
  // while we offset the uv coordinate to the right face.
  switch(index) {
    case 1:
    {
      v += 0.166666;
      break;
    }
    case 2:
    {
      // -Z
      v += 0.166666 * 5;
      break;
    }
    case 3:
    {
      // Z
      v += 0.166666 * 4;
      break;
    }
    case 4:
    {
      // Y
      v += 0.166666 * 2;
      break;
    }
    case 5:
    {
      // -Y
      v += 0.166666 * 3;
      break;
    }
  }

  uv.x = u;
  uv.y = 1.0 - v;
}

void node_tex_environment_empty(vec3 co, out vec4 color)
{
  color = vec4(1.0, 0.0, 1.0, 1.0);
}
