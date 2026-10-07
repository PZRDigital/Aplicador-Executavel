#pragma once

// Skin escura das janelas do jogo: imagens da interface recoloridas (char.grf, skin\) e textos claros.
namespace skin {
	void Install();                     // no início (antes do cliente abrir qualquer imagem)
	void SetTextLightening( bool on );  // clareia os textos escuros (só dentro do jogo)
}
