#pragma once

/// eng::platform::PlatformPaths — raízes conhecidas do sistema hospedeiro
///.
///
/// - Produz apenas RAÍZES (userData/cache/temp/executable) usando
///   fs::Path como value type — platform DEPENDE de fs, nunca o contrário
///   (fronteira da ADR-026).
/// - NÃO conhece estrutura de projeto (isso é eng::project) nem cria
///   diretórios (isso é fs do chamador).
/// - Linux: XDG_DATA_HOME/XDG_CACHE_HOME com fallbacks em $HOME; TMPDIR ou
///   /tmp; executável via /proc/self/exe. Sem XDG e sem HOME: fallback
///   documentado sob o temp (aviso via eng::log).
#include "eng/fs/Path.hpp"

namespace eng::platform {

struct PlatformPaths {
    eng::fs::Path userDataRoot;    ///< dados persistentes do app
    eng::fs::Path cacheRoot;       ///< dados regeneráveis
    eng::fs::Path tempRoot;        ///< arquivos temporários
    eng::fs::Path executableRoot;  ///< diretório do binário em execução

    /// Detecta as raízes para `appName` (subdiretório do app em userData e
    /// cache; temp e executableRoot são compartilhados). Sempre devolve
    /// caminhos não-vazios (fallback documentado acima).
    [[nodiscard]] static PlatformPaths detect(std::string_view appName);
};

} // namespace eng::platform
