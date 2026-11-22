# The MK9 Rollback window's texts in other languages, and which language it shows (2026-11-16, the player: "add
# translation, for now add portuguese brazil"). tools\netplay_gui.ps1 loads this file first; launcher_lib.ps1 and
# updater.ps1 use it through the window (their background jobs load it too).
# Every text a player reads is written in English where it is used, as the argument of Tr:
#   Tr 'Version {0}' $v      -> the translation of 'Version {0}' in the language set by Set-UiLanguage (the English
#                               text itself in English, or when no translation is listed), with {0}, {1}, ... filled in
#                               by PowerShell's -f. Without arguments the text is not formatted (the log-line table of
#                               launcher_lib.ps1 keeps its own {1} {2} {mode}, filled in afterwards).
# The tables below map the English texts to their translations, one pair per line. tools\launcher_selftest.ps1 checks
# that every text the scripts pass to Tr (and every text of launcher_lib's tables) has a translation, that each one has
# the same placeholders and keyboard mark (&) count as its English text, and that no translation is left unused.
# This file is UTF-8 with a byte order mark: Windows PowerShell 5.1 reads a script without one in the PC's ANSI code
# page, which garbles the accents wherever that is not a Western European one. The other scripts stay ASCII.
# The names of MK9's own menus and modes (Online, Player Match, King of the Hill, Ranked) are kept as written in
# English: the game's Portuguese names are not known here.
#   Get-UiLanguageChoice   the language to show: $env:MK9R_LANG (tests), else the player's choice (the window's
#                          Language link: HKCU\Software\MK9Rollback, value Language), else Windows' display language
#   Set-UiLanguage         sets it ('en', 'pt-BR'; any other Portuguese is pt-BR, anything else English)
#   Save-UiLanguageChoice  keeps the player's choice; Get-UiLanguageName: its name in that language
# "MK9 Rollback.exe" (launcher\Launcher.cpp) reads the same choice for its "Starting" card and its messages.

$script:UiLanguages = [ordered]@{ 'en' = 'English'; 'pt-BR' = 'Português (Brasil)' }
$script:UiLang = 'en'
$script:UiTable = $null
$script:UiLangKey = 'HKCU:\Software\MK9Rollback'
# A self-test (MK9R_LAUNCHER_TEST, as in netplay_gui.ps1) keeps its choice apart from the player's.
$script:UiLangValue = if ($env:MK9R_LAUNCHER_TEST) { 'LanguageTest' } else { 'Language' }

function Tr {
    $text = "$($args[0])"
    if ($script:UiTable -and $script:UiTable.ContainsKey($text)) { $text = $script:UiTable[$text] }
    if ($args.Count -gt 1) { return ($text -f @($args[1..($args.Count - 1)])) }
    $text
}

# A language name or code (Windows' 'pt-BR', 'pt-PT', 'en-US', or one of $script:UiLanguages) -> a code of that list.
function Resolve-UiLanguage([string]$name) {
    if (-not $name) { return 'en' }
    foreach ($k in $script:UiLanguages.Keys) { if ($name -eq $k) { return $k } }
    if ($name -match '^(?i)pt([-_].*)?$') { return 'pt-BR' }
    'en'
}

# The pairs of a table -> a lookup that tells 'Play' from 'PLAY' (a hashtable would not).
function New-UiTable($pairs) {
    $d = New-Object 'System.Collections.Generic.Dictionary[string,string]' ([StringComparer]::Ordinal)
    foreach ($p in $pairs) { if ($p.Count -eq 2 -and -not $d.ContainsKey($p[0])) { $d[$p[0]] = $p[1] } }
    $d
}

function Set-UiLanguage([string]$lang) {
    $script:UiLang = Resolve-UiLanguage $lang
    $script:UiTable = if ($script:UiLang -eq 'pt-BR') { New-UiTable $script:UiTextPtBr } else { $null }
}

function Get-UiLanguageChoice([string]$KeyPath = $script:UiLangKey) {
    if ($env:MK9R_LANG) { return (Resolve-UiLanguage $env:MK9R_LANG) }
    try {
        $v = (Get-ItemProperty -LiteralPath $KeyPath -Name $script:UiLangValue -ErrorAction Stop).($script:UiLangValue)
        if ($v) { return (Resolve-UiLanguage "$v") }
    } catch { }
    Resolve-UiLanguage ([Globalization.CultureInfo]::CurrentUICulture.Name)
}

# $true when kept. Only codes of $script:UiLanguages are written.
function Save-UiLanguageChoice([string]$lang, [string]$KeyPath = $script:UiLangKey) {
    $code = Resolve-UiLanguage $lang
    try {
        if (-not (Test-Path -LiteralPath $KeyPath)) { New-Item -Path $KeyPath -Force -ErrorAction Stop | Out-Null }
        Set-ItemProperty -LiteralPath $KeyPath -Name $script:UiLangValue -Value $code -ErrorAction Stop
        $true
    } catch { $false }
}

function Get-UiLanguageName([string]$lang) { $script:UiLanguages[(Resolve-UiLanguage $lang)] }

# ---- Portuguese (Brazil) ----
$script:UiTextPtBr = @(
    # the window
    @('Version {0}', 'Versão {0}'),
    @('Development copy', 'Cópia de desenvolvimento'),
    @('Rollback netcode for Mortal Kombat Komplete Edition', 'Netcode rollback para o Mortal Kombat Komplete Edition'),
    @('Last match: none yet.', 'Última partida: nenhuma ainda.'),
    @('Score: no online match counted yet.', 'Placar: nenhuma partida online contada ainda.'),
    @('&Having trouble?', '&Problemas?'),
    @('&Save logs to the Desktop', '&Salvar os logs'),
    @('&Check for updates', 'Buscar &atualizações'),
    @('&Language: {0}', '&Idioma: {0}'),
    @('&Matches and replays', '&Partidas e replays'),
    @('update available', 'atualização disponível'),
    @('&PLAY', '&JOGAR'),
    @('STARTING...', 'INICIANDO...'),
    @('GAME RUNNING', 'JOGO ABERTO'),
    @('&TRY AGAIN', '&TENTAR DE NOVO'),
    @('CHECKING FOR UPDATES...', 'PROCURANDO ATUALIZAÇÕES...'),
    @('UPDATING...', 'ATUALIZANDO...'),
    @('CLOSE THE GAME TO UPDATE', 'FECHE O JOGO PARA ATUALIZAR'),
    @('&INSTALL THE UPDATE', '&INSTALAR A ATUALIZAÇÃO'),
    @('Try a&gain', 'Tentar de &novo'),
    @('Ready to play', 'Pronto para jogar'),
    @('Press Play: the game starts through Steam with the mod loaded.', 'Pressione Jogar: o jogo abre pela Steam com o mod carregado.'),
    @('Press Play. The game starts with the mod.', 'Pressione Jogar. O jogo abre com o mod.'),
    @('In the game: Online > Player Match, a 1 vs 1 room. Rollback takes over when your opponent runs MK9 Rollback too.', 'No jogo: Online > Player Match, uma sala 1 contra 1. O rollback assume quando o seu oponente também usa o MK9 Rollback.'),
    @('The lines below say why. Then press Play again.', 'As linhas abaixo dizem o motivo. Depois pressione Jogar de novo.'),
    @('The MK9 Rollback window stopped because of an error:', 'A janela do MK9 Rollback parou por causa de um erro:'),
    @('(Saved in {0}; please send it with your bug report.)', '(Salvo em {0}; envie junto com o seu relato do problema.)'),
    @('MK9 Rollback is already open.', 'O MK9 Rollback já está aberto.'),
    @('If you cannot see its window: open Task Manager (Ctrl+Shift+Esc), end "Windows PowerShell" there, then open MK9 Rollback again.', 'Se você não vê a janela dele: abra o Gerenciador de Tarefas (Ctrl+Shift+Esc), finalize o "Windows PowerShell" ali e abra o MK9 Rollback de novo.'),
    @('{0} is missing. Extract the zip again into an empty folder.', '{0} está faltando. Extraia o zip de novo em uma pasta vazia.'),
    @('Something went wrong in this window: {0} (Saved in {1}; please send it with your bug report if it keeps happening.)', 'Algo deu errado nesta janela: {0} (Salvo em {1}; envie junto com o seu relato do problema se continuar acontecendo.)'),
    @('This start is given up. Press Play to try again.', 'Esta tentativa de abrir o jogo foi abandonada. Pressione Jogar para tentar de novo.'),
    # "Having trouble?"
    @('Having trouble?', 'Problemas?'),
    @('If the game stutters, freezes or will not connect:', 'Se o jogo engasgar, congelar ou não conectar:'),
    @('Allow MKKE.exe through the Windows firewall (Windows Security > Firewall > Allow an app).', 'Libere o MKKE.exe no firewall do Windows (Segurança do Windows > Firewall e proteção de rede > Permitir um aplicativo pelo firewall).'),
    @('Add the MK9 Rollback folder to your antivirus exclusions.', 'Adicione a pasta do MK9 Rollback às exclusões do seu antivírus.'),
    @('Run the game from an SSD, and turn off disk sleep.', 'Rode o jogo a partir de um SSD e desative a suspensão do disco.'),
    @('On a laptop: plug it in and set the Windows power mode to "Best performance".', 'Em um notebook: ligue-o na tomada e coloque o modo de energia do Windows em "Melhor desempenho".'),
    @('Close browsers and other heavy apps (downloads, streaming, launchers).', 'Feche navegadores e outros programas pesados (downloads, streaming, launchers).'),
    @('Use a wired (Ethernet) connection instead of Wi-Fi.', 'Use uma conexão por cabo (Ethernet) em vez de Wi-Fi.'),
    @('Both players need the same version: this window installs a new one by itself when it opens.', 'Os dois jogadores precisam da mesma versão: esta janela instala uma nova sozinha quando abre.'),
    @('In a fight, F9 switches between the HUD display and the detailed network line.', 'Durante a luta, F9 alterna entre o indicador no HUD e a linha detalhada da rede.'),
    # match history
    @('Matches and replays', 'Partidas e replays'),
    @('When', 'Quando'),
    @('Result', 'Resultado'),
    @('Rounds', 'Rounds'),
    @('You played', 'Seu lutador'),
    @('Opponent played', 'Lutador do oponente'),
    @('Arena', 'Arena'),
    @('Opponent', 'Oponente'),
    @('yyyy-MM-dd HH:mm', 'dd/MM/yyyy HH:mm'),
    @('Won', 'Vitória'),
    @('Lost', 'Derrota'),
    @('Draw', 'Empate'),
    @('No online match counted yet.', 'Nenhuma partida online contada ainda.'),
    @('A match counts when both players are in a 1 vs 1 Player Match room and it reaches MK9''s results screen.', 'Uma partida conta quando os dois jogadores estão numa sala 1 contra 1 de Player Match e ela chega à tela de resultados do MK9.'),
    @('Against {0}: {1}.', 'Contra {0}: {1}.'),
    @('Show the file (match_history.txt)', 'Mostrar o arquivo (match_history.txt)'),
    @('The match history could not be shown: {0}', 'Não foi possível mostrar o histórico de partidas: {0}'),
    # replays
    @('Replay', 'Replay'),
    @('yes', 'sim'),
    @('&Watch the replay', '&Assistir ao replay'),
    @('Open the replays folder', 'Abrir a pasta dos replays'),
    @('Pick a match marked "yes" in the Replay column, then Watch (or double-click it).', 'Escolha uma partida com "sim" na coluna Replay e clique em Assistir (ou clique duas vezes nela).'),
    @('No replay yet: matches played with version 20261117-1517 or later have one.', 'Nenhum replay ainda: as partidas jogadas com a versão 20261117-1517 ou mais nova têm um.'),
    @('This replay file is gone.', 'Este arquivo de replay não existe mais.'),
    @('Press PLAY first: a replay plays in the game with the mod. Then choose it here again.', 'Pressione JOGAR primeiro: o replay passa no jogo com o mod. Depois escolha-o aqui de novo.'),
    @('The game could not be told (the command file is busy). Try again.', 'Não foi possível avisar o jogo (o arquivo de comandos está ocupado). Tente de novo.'),
    @('Ready: in the game, start an offline fight (Ladder or Versus) and the replay plays in it. Not in an online room.', 'Pronto: no jogo, comece uma luta offline (Ladder ou Versus) e o replay passa nela. Não numa sala online.'),
    @('Replay {0} is ready: start an offline fight (Ladder or Versus) in the game and it plays there.', 'O replay {0} está pronto: comece uma luta offline (Ladder ou Versus) no jogo e ele passa nela.'),
    # language
    @('The language could not be saved.', 'Não foi possível salvar o idioma.'),
    @('This window shows {0} the next time it opens.', 'Esta janela aparece em {0} na próxima vez que abrir.'),
    # updates
    @('Checking for updates...', 'Procurando atualizações...'),
    @('no answer', 'sem resposta'),
    @('The update check failed ({0}). You can play; the window asks again before the next start.', 'A busca por atualizações falhou ({0}). Você pode jogar; a janela verifica de novo antes do próximo início.'),
    @('Version {0} is out. A development copy is not updated by itself.', 'A versão {0} saiu. Uma cópia de desenvolvimento não se atualiza sozinha.'),
    @('UPDATE: version {0} is out. It is installed now, by itself; your opponent needs it too.', 'ATUALIZAÇÃO: a versão {0} saiu. Ela é instalada agora, automaticamente; o seu oponente também precisa dela.'),
    @('You have the latest version ({0}).', 'Você tem a versão mais recente ({0}).'),
    @('Version {0} is available', 'A versão {0} está disponível'),
    @('Version {0} cannot be installed into a read-only folder. Move the MK9 Rollback folder to e.g. C:\Games, then open it again.', 'A versão {0} não pode ser instalada em uma pasta somente leitura. Mova a pasta do MK9 Rollback para, por exemplo, C:\Games e abra de novo.'),
    @('Version {0} is available: it installs once the game is closed', 'A versão {0} está disponível: ela é instalada quando o jogo fechar'),
    @('Close the game: version {0} then installs by itself. Your opponent needs it too.', 'Feche o jogo: a versão {0} então é instalada automaticamente. O seu oponente também precisa dela.'),
    @('Close the game to update', 'Feche o jogo para atualizar'),
    @('The new version installs by itself once the game is closed. Then press Play.', 'A nova versão é instalada automaticamente quando o jogo fechar. Depois pressione Jogar.'),
    @('Installing version {0}...', 'Instalando a versão {0}...'),
    @('This window restarts by itself when the update is in. Then press Play.', 'Esta janela reinicia sozinha quando a atualização terminar. Depois pressione Jogar.'),
    @('Finishing the update: fetching "MK9 Rollback.exe", the program that opens this window from now on...', 'Concluindo a atualização: baixando o "MK9 Rollback.exe", o programa que abre esta janela daqui em diante...'),
    @('Done. From now on, open MK9 Rollback with "MK9 Rollback.exe" in its folder ("Play Online.bat" is gone).', 'Pronto. Daqui em diante, abra o MK9 Rollback pelo "MK9 Rollback.exe" na pasta dele (o "Play Online.bat" foi removido).'),
    @('It could not be added now{0}; it is tried again the next time this window opens. Until then "Play Online.bat" still opens it.', 'Não foi possível adicioná-lo agora{0}; ele é tentado de novo na próxima vez que esta janela abrir. Até lá, o "Play Online.bat" ainda abre a janela.'),
    @('Restarting this window...', 'Reiniciando esta janela...'),
    @('This window could not restart itself. Close it and open MK9 Rollback again.', 'Esta janela não conseguiu reiniciar sozinha. Feche-a e abra o MK9 Rollback de novo.'),
    @('Open MK9 Rollback again', 'Abra o MK9 Rollback de novo'),
    @('The update is in. Close this window and open MK9 Rollback again to play.', 'A atualização foi instalada. Feche esta janela e abra o MK9 Rollback de novo para jogar.'),
    @('Close the game first: version {0} then installs by itself.', 'Feche o jogo primeiro: a versão {0} então é instalada automaticamente.'),
    @('The update failed: {0}', 'A atualização falhou: {0}'),
    @('Press Try again. Or download version {0} yourself from {1} and extract it over this folder.', 'Pressione Tentar de novo. Ou baixe a versão {0} você mesmo em {1} e extraia por cima desta pasta.'),
    @('Version {0} was not installed', 'A versão {0} não foi instalada'),
    @('The update was not installed', 'A atualização não foi instalada'),
    @('The lines below say why. Play stays off until the new version is in: press the button to try again.', 'As linhas abaixo dizem o motivo. Jogar fica bloqueado até a nova versão ser instalada: pressione o botão para tentar de novo.'),
    @('The update is still being installed: this window closes by itself when it is done. Please wait.', 'A atualização ainda está sendo instalada: esta janela fecha sozinha quando terminar. Aguarde.'),
    # logs
    @('Saving the logs...', 'Salvando os logs...'),
    @('The logs zip could not be made ({0}).', 'Não foi possível criar o zip dos logs ({0}).'),
    @('The logs zip could not be made.', 'Não foi possível criar o zip dos logs.'),
    @('No logs yet (or they could not be read).', 'Ainda não há logs (ou não foi possível lê-los).'),
    @('Logs saved: {0}. Steam IDs, IP addresses and your user and PC names in them were replaced (PRIVACY.txt in the zip lists what). Send it together with your opponent''s zip.', 'Logs salvos: {0}. Os IDs da Steam, os endereços IP e os seus nomes de usuário e do PC foram substituídos neles (o PRIVACY.txt no zip lista o quê). Envie junto com o zip do seu oponente.'),
    # starting the game
    @('Starting Mortal Kombat through Steam...', 'Abrindo o Mortal Kombat pela Steam...'),
    @('Starting the game...', 'Abrindo o jogo...'),
    @('Steam is starting Mortal Kombat. This can take a minute.', 'A Steam está abrindo o Mortal Kombat. Isso pode levar um minuto.'),
    @('The game is already running. Loading the mod...', 'O jogo já está aberto. Carregando o mod...'),
    @('The game is already running.', 'O jogo já está aberto.'),
    @('Start Steam and sign in first.', 'Abra a Steam e entre na sua conta primeiro.'),
    @('Steam is not running', 'A Steam não está aberta'),
    @('Start Steam and sign in, then press Play.', 'Abra a Steam e entre na sua conta, depois pressione Jogar.'),
    @('The mod files are missing next to this launcher ({0}).', 'Os arquivos do mod não estão junto deste launcher ({0}).'),
    @('The mod''s files are damaged', 'Os arquivos do mod estão danificados'),
    @('This folder is read-only (or protected by Windows ransomware protection). Move the MK9 Rollback folder to e.g. C:\Games.', 'Esta pasta é somente leitura (ou protegida pela proteção contra ransomware do Windows). Mova a pasta do MK9 Rollback para, por exemplo, C:\Games.'),
    @('This folder cannot be written', 'Não é possível gravar nesta pasta'),
    @('Move the MK9 Rollback folder to e.g. C:\Games, then open it again.', 'Mova a pasta do MK9 Rollback para, por exemplo, C:\Games e abra de novo.'),
    @('The game is running. Waiting for its menu...', 'O jogo está aberto. Esperando o menu dele...'),
    @('Waiting for the game''s menu...', 'Esperando o menu do jogo...'),
    @('The mod is loaded once the game has reached its menu.', 'O mod é carregado quando o jogo chega ao menu.'),
    @('The game did not start. Start it from Steam, then press Play again.', 'O jogo não abriu. Abra-o pela Steam e pressione Jogar de novo.'),
    @('Loading the mod...', 'Carregando o mod...'),
    @('Two copies of the game are running. Close one.', 'Há duas cópias do jogo abertas. Feche uma.'),
    @('The mod was already loaded in this game.', 'O mod já estava carregado neste jogo.'),
    @('Mod loaded.', 'Mod carregado.'),
    @('The game did not start with the mod', 'O jogo não abriu com o mod'),
    @('Ready in the game', 'Pronto no jogo'),
    @('Ready. In the game: Online > Player Match, a 1 vs 1 room or a King of the Hill room with just the two of you. Play anyone there.', 'Pronto. No jogo: Online > Player Match, uma sala 1 contra 1 ou uma sala King of the Hill só com vocês dois. Jogue contra qualquer pessoa ali.'),
    @('If your opponent also runs MK9 Rollback, rollback takes over at the start of the fight. Against anyone else, MK9''s own netcode is used. Ranked is never touched.', 'Se o seu oponente também usa o MK9 Rollback, o rollback assume no início da luta. Contra qualquer outra pessoa, é usado o netcode do próprio MK9. O Ranked nunca é alterado.'),
    @('Rollback is on', 'Rollback ligado'),
    @('The game has closed.', 'O jogo foi fechado.'),
    @('The game is not responding (frozen for more than 15 seconds). If it stays frozen, close it, then save the logs.', 'O jogo não está respondendo (travado há mais de 15 segundos). Se continuar travado, feche-o e salve os logs.'),
    @('The game is not responding', 'O jogo não está respondendo'),
    @('The game is responding again.', 'O jogo voltou a responder.'),
    @('The mod did not answer. Press TRY AGAIN; if it repeats, close the game, press Play, and save the logs.', 'O mod não respondeu. Pressione TENTAR DE NOVO; se repetir, feche o jogo, pressione Jogar e salve os logs.'),
    @('The mod is not answering', 'O mod não está respondendo'),
    @('Press TRY AGAIN. If it repeats, close the game and press Play.', 'Pressione TENTAR DE NOVO. Se repetir, feche o jogo e pressione Jogar.'),
    # the injector (launcher_lib)
    @('This game version is not supported (it must be the Steam Komplete Edition, unmodified).', 'Esta versão do jogo não é suportada (precisa ser o Komplete Edition da Steam, sem modificações).'),
    @('MK9Rollback.dll is missing (often removed by antivirus). Restore it from quarantine or extract the zip again, then add the folder to the exclusions.', 'O MK9Rollback.dll está faltando (muitas vezes removido pelo antivírus). Restaure-o da quarentena ou extraia o zip de novo, depois adicione a pasta às exclusões.'),
    @('The game runs as administrator. Close it and start Steam normally.', 'O jogo está rodando como administrador. Feche-o e abra a Steam normalmente.'),
    @('Windows or your antivirus refused to load MK9Rollback.dll.', 'O Windows ou o seu antivírus recusou carregar o MK9Rollback.dll.'),
    @('The game did not answer while the mod was loading.', 'O jogo não respondeu enquanto o mod carregava.'),
    @('MK9RollbackInjector.exe is missing (often removed by antivirus). Restore it from quarantine or extract the zip again, then add the folder to the exclusions.', 'O MK9RollbackInjector.exe está faltando (muitas vezes removido pelo antivírus). Restaure-o da quarentena ou extraia o zip de novo, depois adicione a pasta às exclusões.'),
    @('Windows could not start MK9RollbackInjector.exe ({0}). Your antivirus may be blocking it: add the MK9 Rollback folder to its exclusions, then press Play again.', 'O Windows não conseguiu iniciar o MK9RollbackInjector.exe ({0}). O seu antivírus pode estar bloqueando: adicione a pasta do MK9 Rollback às exclusões dele e pressione Jogar de novo.'),
    @('The game was closed before the mod was loaded. Press Play to start it again.', 'O jogo foi fechado antes de o mod ser carregado. Pressione Jogar para abri-lo de novo.'),
    @('Could not load the mod into the game. Your antivirus may have blocked MK9RollbackInjector.exe. Press ''Having trouble?'' for what to try, then save the logs if it keeps failing.', 'Não foi possível carregar o mod no jogo. O seu antivírus pode ter bloqueado o MK9RollbackInjector.exe. Pressione ''Problemas?'' para ver o que tentar e salve os logs se continuar falhando.'),
    @('MK9 Rollback''s files are damaged or mixed (MK9Rollback.dll). Extract the zip again into an empty folder.', 'Os arquivos do MK9 Rollback estão danificados ou misturados (MK9Rollback.dll). Extraia o zip de novo em uma pasta vazia.'),
    # last match, score
    @('Last match: {0}.', 'Última partida: {0}.'),
    @('input delay {0}', 'atraso {0}'),
    @('ping {0} ms', 'ping {0} ms'),
    @('{0} rollbacks a minute', '{0} rollbacks/min'),
    @('out of sync in {0} fight(s)', 'fora de sincronia em {0} luta(s)'),
    @('always in sync', 'sempre em sincronia'),
    @('{0} win', '{0} vitória'),
    @('{0} wins', '{0} vitórias'),
    @('{0} loss', '{0} derrota'),
    @('{0} losses', '{0} derrotas'),
    @('{0} draw', '{0} empate'),
    @('{0} draws', '{0} empates'),
    @('Match over: you won{0}.', 'Fim da partida: você venceu{0}.'),
    @('Match over: you lost{0}.', 'Fim da partida: você perdeu{0}.'),
    @('Match over: a draw.', 'Fim da partida: empate.'),
    @('Score today: {0}.', 'Placar de hoje: {0}.'),
    @('Score today: {0}. All time: {1}.', 'Placar de hoje: {0}. No total: {1}.'),
    @('Score: none today. All time: {0}.', 'Placar: nenhuma hoje. No total: {0}.'),
    @('player ...{0}', 'jogador ...{0}'),
    @('You are on Wi-Fi: a cable gives steadier fights.', 'Você está no Wi-Fi: um cabo deixa as lutas mais estáveis.'),
    # the updater
    @('GitHub is limiting update checks; try again in an hour.', 'O GitHub está limitando as buscas por atualizações; tente de novo em uma hora.'),
    @('could not reach GitHub ({0})', 'não foi possível acessar o GitHub ({0})'),
    @('unexpected release tag ''{0}''', 'tag de versão inesperada ''{0}'''),
    @('release {0} has no zip and checksum', 'a versão {0} não tem zip e checksum'),
    @('Downloading version {0}...', 'Baixando a versão {0}...'),
    @('The download failed: {0}. Nothing was changed.', 'O download falhou: {0}. Nada foi alterado.'),
    @('The download failed: {0}.', 'O download falhou: {0}.'),
    @('GitHub is limiting downloads; try again in an hour. Nothing was changed.', 'O GitHub está limitando os downloads; tente de novo em uma hora. Nada foi alterado.'),
    @('The download is damaged (checksum mismatch). Nothing was changed; try again later.', 'O download está danificado (checksum diferente). Nada foi alterado; tente de novo mais tarde.'),
    @('The download is damaged (checksum mismatch). Nothing was changed.', 'O download está danificado (checksum diferente). Nada foi alterado.'),
    @('The download holds a file an update must not have ({0}). Nothing was changed.', 'O download contém um arquivo que uma atualização não pode ter ({0}). Nada foi alterado.'),
    @('The download is not a complete package. Nothing was changed.', 'O download não é um pacote completo. Nada foi alterado.'),
    @('The download is not this version''s package. Nothing was changed.', 'O download não é o pacote desta versão. Nada foi alterado.'),
    @('The update failed: {0}. Your files are as they were.', 'A atualização falhou: {0}. Os seus arquivos estão como antes.'),
    @('The update failed: {0}. Nothing was changed.', 'A atualização falhou: {0}. Nada foi alterado.'),
    @('Updated to version {0}. The previous files are in previous_version.', 'Atualizado para a versão {0}. Os arquivos anteriores estão em previous_version.'),
    @('Updated to version {0}. The previous files are in previous_version.new (the old previous_version folder could not be removed).', 'Atualizado para a versão {0}. Os arquivos anteriores estão em previous_version.new (não foi possível remover a pasta previous_version antiga).'),
    @('Close the game first: its files cannot be replaced while it runs.', 'Feche o jogo primeiro: os arquivos não podem ser substituídos enquanto ele está aberto.'),
    @('It could not be added: {0}.', 'Não foi possível adicioná-lo: {0}.'),
    @('There is no previous version to go back to.', 'Não há versão anterior para voltar.'),
    @('The previous_version folder is empty. Nothing was changed.', 'A pasta previous_version está vazia. Nada foi alterado.'),
    @('Going back failed: {0}. Your files are as they were.', 'A volta falhou: {0}. Os seus arquivos estão como antes.'),
    @('Back to version {0}.', 'De volta à versão {0}.'),
    @('(unknown)', '(desconhecida)'),
    # the game's log lines (launcher_lib $script:LauncherMessages); {mode} is Rollback (or Safe mode)
    @('Rollback', 'Rollback'),
    @('Safe mode', 'Modo seguro'),
    @('Your opponent is not running the mod: MK9''s own netcode plays this match. Send them the download link.', 'O seu oponente não está usando o mod: o netcode do próprio MK9 joga esta partida. Envie o link de download para o seu oponente.'),
    @('Your opponent has no mod: MK9''s own netcode', 'O seu oponente não tem o mod: netcode do MK9'),
    @('The mod could not hook this game version, so MK9''s own netcode plays. Please save the logs and report it.', 'O mod não conseguiu se conectar a esta versão do jogo, então o netcode do próprio MK9 joga. Salve os logs e relate o problema.'),
    @('The mod could not hook the game', 'O mod não conseguiu se conectar ao jogo'),
    @('Online fight found. Checking whether your opponent runs the mod...', 'Luta online encontrada. Verificando se o seu oponente usa o mod...'),
    @('Opponent found: checking for the mod...', 'Oponente encontrado: verificando o mod...'),
    @('Your opponent''s mod is older: they must close the game and open MK9 Rollback again, which updates it. MK9''s netcode plays this match.', 'O mod do seu oponente é mais antigo: o oponente precisa fechar o jogo e abrir o MK9 Rollback de novo, que atualiza o mod. O netcode do MK9 joga esta partida.'),
    @('Your opponent needs to update', 'O seu oponente precisa atualizar'),
    @('Your mod is older than your opponent''s: close the game, and this window installs the new version by itself. MK9''s netcode plays this match.', 'O seu mod é mais antigo que o do oponente: feche o jogo, e esta janela instala a nova versão automaticamente. O netcode do MK9 joga esta partida.'),
    @('You need to update', 'Você precisa atualizar'),
    @('You and your opponent run different builds of the mod: both extract the same zip. MK9''s netcode plays this match.', 'Você e o seu oponente usam builds diferentes do mod: os dois devem extrair o mesmo zip. O netcode do MK9 joga esta partida.'),
    @('You and your opponent need the same version', 'Você e o seu oponente precisam da mesma versão'),
    @('Your game files differ from your opponent''s: both need the unmodified Steam Komplete Edition (Steam: Properties, Installed Files, Verify). MK9''s netcode plays this match.', 'Os arquivos do seu jogo são diferentes dos do oponente: os dois precisam do Komplete Edition da Steam sem modificações (Steam: Propriedades, Arquivos instalados, Verificar integridade). O netcode do MK9 joga esta partida.'),
    @('Your mod''s settings differ from your opponent''s: both close the game and start it from this window again. MK9''s netcode plays this match.', 'As configurações do seu mod são diferentes das do oponente: os dois devem fechar o jogo e abri-lo por esta janela de novo. O netcode do MK9 joga esta partida.'),
    @('Your opponent''s mod answered: {mode} can take over.', 'O mod do seu oponente respondeu: {mode} pode assumir.'),
    @('Both games run the mod.', 'Os dois jogos usam o mod.'),
    @('Both games run the mod', 'Os dois jogos usam o mod'),
    @('This match is not a 1 vs 1 Player/Private match or a King of the Hill room with just the two players (Ranked, or more players in the room): MK9''s own netcode plays this match.', 'Esta partida não é uma partida 1 contra 1 Player/Private nem uma sala King of the Hill só com os dois jogadores (Ranked, ou mais jogadores na sala): o netcode do próprio MK9 joga esta partida.'),
    @('Not a 1 vs 1 Player Match: MK9''s own netcode', 'Não é Player Match 1 contra 1: netcode do MK9'),
    @('Your opponent''s mod runs in {1} and yours in {2}: both must update MK9 Rollback and start the game from its window. MK9''s netcode plays until then.', 'O mod do seu oponente roda em {1} e o seu em {2}: os dois precisam atualizar o MK9 Rollback e abrir o jogo pela janela dele. O netcode do MK9 joga até lá.'),
    @('Your opponent''s mod is set up differently', 'O mod do seu oponente está configurado de outro jeito'),
    @('{mode} takes over when this fight starts.', '{mode} assume quando esta luta começar.'),
    @('This fight already started: {mode} takes over from the next fight.', 'Esta luta já começou: {mode} assume a partir da próxima luta.'),
    @('Fight over. {mode} stays on for the next fight.', 'Fim da luta. {mode} continua ligado para a próxima luta.'),
    @('{mode} on. You are player {1}.', '{mode} ligado. Você é o jogador {1}.'),
    @('Input delay this match: {1} frame(s).', 'Atraso de entrada nesta partida: {1} frame(s).'),
    @('Input delay: {1} frames from this round (the ping changed).', 'Atraso de entrada: {1} frames a partir deste round (o ping mudou).'),
    @('Match over. The results and rematch screens run on MK9''s netcode; {mode} takes over again at the next fight.', 'Fim da partida. As telas de resultado e revanche usam o netcode do MK9; {mode} assume de novo na próxima luta.'),
    @('Back to MK9''s own netcode ({1}).', 'De volta ao netcode do próprio MK9 ({1}).'),
    @('Steam is not available to the game. Is Steam running and signed in?', 'A Steam não está disponível para o jogo. A Steam está aberta e com a conta conectada?'),
    @('Fight loaded. Waiting for your opponent to start the same fight...', 'Luta carregada. Esperando o seu oponente começar a mesma luta...'),
    @('Connected to your opponent.', 'Conectado ao seu oponente.'),
    @('FIGHT! Both games are running.', 'FIGHT! Os dois jogos estão rodando.'),
    @('Game files and mod version match your opponent''s.', 'Os arquivos do jogo e a versão do mod batem com os do seu oponente.'),
    @('Your setup differs from your opponent''s: {1}. Both need the newest mod version (this window installs it when it opens) and an unmodified game.', 'A sua instalação é diferente da do oponente: {1}. Os dois precisam da versão mais nova do mod (esta janela a instala quando abre) e do jogo sem modificações.'),
    @('The two fights don''t match. Pick the same characters on the same sides, the same stage and settings, then fight again.', 'As duas lutas não batem. Escolham os mesmos personagens nos mesmos lados, a mesma arena e as mesmas opções, e lutem de novo.'),
    @('OUT OF SYNC at frame {1}: the two screens may now differ. Please save the logs after this fight.', 'FORA DE SINCRONIA no frame {1}: as duas telas podem estar diferentes agora. Salve os logs depois desta luta.'),
    @('Out of sync', 'Fora de sincronia'),
    @('The two games went out of sync: the rest of this fight runs on MK9''s own netcode. Please save the logs after it.', 'Os dois jogos saíram de sincronia: o resto desta luta usa o netcode do próprio MK9. Salve os logs depois dela.'),
    @('Out of sync: MK9''s own netcode until the next fight', 'Fora de sincronia: netcode do MK9 até a próxima luta'),
    @('Fight over: {1} frames in sync, {2} out of sync.', 'Fim da luta: {1} frames em sincronia, {2} fora de sincronia.'),
    @('Your opponent''s game did not join in time. This fight continues on MK9''s own netcode.', 'O jogo do seu oponente não entrou a tempo. Esta luta continua no netcode do próprio MK9.'),
    @('The connection to your opponent could not be set up (a network port may be in use). This fight continues on MK9''s own netcode.', 'Não foi possível montar a conexão com o seu oponente (uma porta de rede pode estar em uso). Esta luta continua no netcode do próprio MK9.'),
    @('Your opponent disconnected. The rest of this fight continues on MK9''s own netcode; the next fight tries again.', 'O seu oponente desconectou. O resto desta luta continua no netcode do próprio MK9; a próxima luta tenta de novo.'),
    @('Your opponent disconnected', 'O seu oponente desconectou'),
    @('The mod reported a crash. Please save the logs.', 'O mod relatou um crash. Salve os logs.'),
    @('The mod reported a crash', 'O mod relatou um crash'),
    @('The game stopped at an error box (it may be hidden behind the game). Please save the logs.', 'O jogo parou em uma caixa de erro (ela pode estar escondida atrás do jogo). Salve os logs.'),
    @('The game stopped at an error box', 'O jogo parou em uma caixa de erro')
)
