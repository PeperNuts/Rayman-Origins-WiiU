// Rayman Origins Wii U port - controller-specific texts.
//
// The GamePad, Pro Controller and Classic Controller all reach the game as a Classic
// Controller. For the message that names the controller style (text 4049, "The controls are
// in Classic style."), LocalisationManager::getText is pointed here (tools/game_patch.py) and
// asks for text 9049 (GamePad) or 9050 (Pro Controller) instead, which tools/loc_patch.py adds
// to the localisation file in sd:/rayman/mods. Without that file the original text is used.
#include <stdint.h>

typedef void (*GetTextFn)(void *ret, void *tmpl, const uint32_t *id, uint32_t lang);
// Distance from port_loc_gettext to Localisation_Template::getText (a local symbol of the game
// object), filled in after the link by tools/game_patch.py. A distance, because the loader
// moves the code: an absolute address patched in after the link would not be relocated.
volatile uint32_t port_loc_template_gettext __attribute__((used)) = 0xDEAD0001u;

extern volatile int port_pad_kind;
volatile int port_loc_has_pad_texts; // set by wii_dvd.c when the edited localisation file is used

// Same arguments as Localisation_Template::getText (called from game code, game r2/r13 live).
void port_loc_gettext(void *ret, void *tmpl, const uint32_t *id, uint32_t lang)
{
   uint32_t alt;
   const uint32_t *use = id;
   if (*id == 4049 && port_loc_has_pad_texts && (port_pad_kind == 1 || port_pad_kind == 2)) {
      alt = port_pad_kind == 1 ? 9049 : 9050;
      use = &alt;
   }
   ((GetTextFn)((uint32_t)port_loc_gettext + port_loc_template_gettext))(ret, tmpl, use, lang);
}
