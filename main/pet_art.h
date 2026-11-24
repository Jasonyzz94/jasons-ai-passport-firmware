// main/pet_art.h —— 像素宠物的字符画(按物种、成长阶段和形态取图)。
#pragma once

#include "pet.h"
#include "pixel_types.h"

const pixel_sprite_t *pet_art(pet_species_t sp, pet_stage_t stage, pet_form_t form);
// 图鉴里还没见过的形态:暗色剪影。
const pixel_sprite_t *pet_art_silhouette(pet_species_t sp);
extern const pixel_sprite_t PET_ICON;
