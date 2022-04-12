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

int node_tex_environment_cubemap_xyz_to_uv(vec3 co, out float u, out float v, out float max_axis)
{
  float abs_x = abs(co.x);
  float abs_y = abs(co.y);
  float abs_z = abs(co.z);
  
  bool is_x_positive = co.x > 0.0 ? true : false;
  bool is_y_positive = co.y > 0.0 ? true : false;
  bool is_z_positive = co.z > 0.0 ? true : false;

  int index = -1;

  if (is_x_positive && abs_x >= abs_y && abs_x >= abs_z) {
    max_axis = abs_x;
    u = co.y;
    v = co.z;
    index = 0;
  }
 if (!is_x_positive && abs_x >= abs_y && abs_x >= abs_z) {
    max_axis = abs_x;
    u = -co.y;
    v = co.z;
    index = 1;
  }
 if (is_y_positive && abs_y >= abs_x && abs_y >= abs_z) {
    max_axis = abs_y;
    u = -co.x;
    v = co.z;
    index = 2;
  }
 if (!is_y_positive && abs_y >= abs_x && abs_y >= abs_z) {
    max_axis = abs_y;
    u = co.x;
    v = co.z;
    index = 3;
  }
 if (is_z_positive && abs_z >= abs_x && abs_z >= abs_y) {
    max_axis = abs_z;
    u = co.x;
    v = co.y;
    index = 4;
  }
 if (!is_z_positive && abs_z >= abs_x && abs_z >= abs_y) {
    max_axis = abs_z;
    u = co.x;
    v = co.y;
    index = 5;
  }

  return index;
}

/* As Blender's user facing coordinate system is different from the backend's coordinate system (OpenGL)
 * the order of various sides of the cube are handled in the appropriate order.
 * So `[X, -X, Y, -Y, Z, -Z]` becomes `[X, -X, -Z, Z, Y, -Y]`. */

void node_tex_environment_cubemap_cross_horizontal(vec3 co, out vec3 uv)
{
  float max_axis = 0.0;
  float uc = 0.0;
  float vc = 0.0;
  int index = node_tex_environment_cubemap_xyz_to_uv(co, uc, vc, max_axis);
  /* Convert u range from -1 to 1 to 0 to 0.25 (1/4) as the texture space is 4 faces wide. */
  float u = 0.125 * (uc / max_axis + 1.0);
  /* Convert v range from -1 to 1 to 0 to 0.333.. (1/3) as the texture space is 3 faces high. */
  float v = 0.166667 * (vc / max_axis + 1.0);

  switch(index) {
    case 0:
    {
      u += 0.5;
      v += 0.333333;
      break;
    }
    case 1:
    {
      v += 0.333333;
      break;
    }
    case 2:
    {
      u += 0.75;
      v += 0.333333;
      break;
    }
    case 3:
    {
      u += 0.25;
      v += 0.333333;
      break;
    }
    case 4:
    {
      u += 0.25;
      v += 0.666666;
      break;
    }
    case 5:
    {
      u += 0.25;
      v = 0.333333 - v;
      break;
    }
  }

  uv.x = u;
  uv.y = v;
}

void node_tex_environment_cubemap_stripe_horizontal(vec3 co, out vec3 uv)
{
  float max_axis = 0.0;
  float uc = 0.0;
  float vc = 0.0;
  int index = node_tex_environment_cubemap_xyz_to_uv(co, uc, vc, max_axis);
  /* Convert u range from -1 to 1 to 0 to 0.166667 (1/6) as the texture space is 6 faces wide. */
  float u = 0.083333 * (uc / max_axis + 1.0);
  /* Convert v range from -1 to 1 to 0 to 1 as the texture space is 1 face high. */
  float v = 0.5 * (vc / max_axis + 1.0);

  switch(index) {
    case 1:
    {
      u += 0.166666;
      break;
    }
    case 2:
    {
      u += 0.166666 * 5;
      break;
    }
    case 3:
    {
      u += 0.166666 * 4;
      break;
    }
    case 4:
    {
      u += 0.166666 * 2;
      break;
    }
    case 5:
    {
      u += 0.166666 * 3;
      v = 1.0 - v;
      break;
    }
  }

  uv.x = u;
  uv.y = v;
}

void node_tex_environment_cubemap_stripe_vertical(vec3 co, out vec3 uv)
{
  float max_axis = 0.0;
  float uc = 0.0;
  float vc = 0.0;
  int index = node_tex_environment_cubemap_xyz_to_uv(co, uc, vc, max_axis);
  /* Convert v range from -1 to 1 to 0 to 1 as the texture space is 1 face high. */
  float u = 0.5 * (uc / max_axis + 1.0);
  /* Convert u range from -1 to 1 to 0 to 0.166667 (1/6) as the texture space is 6 faces high. */
  float v = 0.083333 * (vc / max_axis + 1.0);

  switch(index) {
    case 0:
    {
      v += 0.166666 * 5;
      break;
    }
    case 1:
    {
      v += 0.166666 * 4;
      break;
    }
    case 3:
    {
      v += 0.166666;
      break;
    }
    case 4:
    {
      v += 0.166666 * 3;
      break;
    }
    case 5:
    {
      v = 0.166666 * 3 - v;
      break;
    }
  }

  uv.x = u;
  uv.y = v;
}

void node_tex_environment_empty(vec3 co, out vec4 color)
{
  color = vec4(1.0, 0.0, 1.0, 1.0);
}
