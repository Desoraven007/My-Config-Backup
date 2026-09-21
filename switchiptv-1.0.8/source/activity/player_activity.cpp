#include "activity/player_activity.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>

#include "data/storage.hpp"
#include "data/stream_url.hpp"
#include "ui/mpv_view.hpp"

namespace {

std::string formatEpgTime(const std::string& datetime) {
    size_t spacePos = datetime.find(' ');
    if (spacePos != std::string::npos && datetime.size() >= spacePos + 6) {
        std::string timePart = datetime.substr(spacePos + 1, 5);
        if (timePart.size() == 5 && timePart[2] == ':') return timePart;
    }

    // Alguns provedores mandam timestamp Unix cru (só dígitos) em vez de
    // data formatada — mesmo bug corrigido no Android/web.
    bool allDigits = !datetime.empty() && std::all_of(datetime.begin(), datetime.end(), [](unsigned char c) { return std::isdigit(c); });
    if (allDigits) {
        time_t epochSeconds = static_cast<time_t>(std::stoll(datetime));
        struct tm tmResult{};
        localtime_r(&epochSeconds, &tmResult);
        char buf[8];
        snprintf(buf, sizeof(buf), "%02d:%02d", tmResult.tm_hour, tmResult.tm_min);
        return std::string(buf);
    }
    return datetime;
}

std::string formatPlayerTime(double seconds) {
    if (seconds < 0) seconds = 0;
    int totalSeconds = static_cast<int>(seconds);
    int mins = totalSeconds / 60;
    int secs = totalSeconds % 60;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", mins, secs);
    return std::string(buf);
}

}  // namespace

PlayerActivity::PlayerActivity(iptv::StoredAuth auth, iptv::CatalogItem item,
                                std::vector<iptv::CatalogItem> liveChannels)
    : auth(std::move(auth)),
      item(std::move(item)),
      liveChannels(std::move(liveChannels)),
      isLive(this->item.kind == iptv::ContentKind::Live),
      fullscreen(!isLive) {}

PlayerActivity::~PlayerActivity() {
    *alive = false;
}

brls::View* PlayerActivity::createContentView() {
    rootBox = new brls::Box();
    rootBox->setAxis(brls::Axis::COLUMN);
    rootBox->setFocusable(true);

    headerBox = new brls::Box();
    headerBox->setAxis(brls::Axis::ROW);
    headerBox->setHeight(60);
    headerBox->setPadding(12, 20, 12, 20);

    titleLabel = new brls::Label();
    titleLabel->setFontSize(22);
    titleLabel->setGrow(1.0f);
    titleLabel->setText(item.name);
    headerBox->addView(titleLabel);

    hintLabel = new brls::Label();
    hintLabel->setFontSize(16);
    hintLabel->setText(isLive ? "Cima/Baixo: trocar canal   A: tela cheia   X: áudio/legendas   B: voltar"
                               : "A: pausar/tocar   Esquerda/Direita: -/+10s   X: áudio/legendas   B: voltar");
    headerBox->addView(hintLabel);

    rootBox->addView(headerBox);

    brls::Box* videoWrapperBox = new brls::Box();
    videoWrapperBox->setAxis(brls::Axis::ROW);
    videoWrapperBox->setJustifyContent(brls::JustifyContent::CENTER);
    videoWrapperBox->setWidth(brls::Application::contentWidth);

    videoBox = new brls::Box();
    mpvView = new MpvView();
    // videoBox é ROW (padrão do Box) — grow preenche a largura (eixo
    // principal); a altura (eixo cruzado) já estica sozinha por padrão. Sem
    // isso, mpvView fica com largura zero e `draw()` nunca roda de verdade.
    mpvView->setGrow(1.0f);
    mpvView->setOnProgress([this](double position, double duration) {
        if (!playbackStarted) {
            playbackStarted = true;
            loadingLabel->setText("Carregando...");
            loadingLabel->setVisibility(brls::Visibility::GONE);
            loadingSpinner->setVisibility(brls::Visibility::GONE);
            errorLabel->setVisibility(brls::Visibility::GONE);
            // Retomada pendente: só agora, com o stream comprovadamente
            // aberto e tocando, é que o seek pra posição salva é confiável
            // (ver comentário em `playCurrentItem` pro porquê de não pedir
            // pro mpv já abrir direto numa posição).
            if (pendingResumeSeconds > 0) {
                mpvView->player()->seekAbsolute(pendingResumeSeconds);
                pendingResumeSeconds = -1;
            }
            // Reprodução de fato começou — mostra o OSD uma vez (some
            // sozinho em 4s se o usuário não mexer).
            showOsd();
            armRecoveryWatch();
        }
        updateTimeLabel(position, duration);
        // Ao vivo não tem "posição" que faça sentido retomar depois.
        if (item.kind == iptv::ContentKind::Live) return;
        iptv::updateProgress(item.id, position, duration);
    });
    mpvView->setOnError([this](const std::string& message) {
        loadingLabel->setVisibility(brls::Visibility::GONE);
        loadingSpinner->setVisibility(brls::Visibility::GONE);
        errorLabel->setText("Erro na reprodução: " + message);
        errorLabel->setVisibility(brls::Visibility::VISIBLE);
    });
    videoBox->addView(mpvView);

    // Coluna sobreposta (cobre o vídeo inteiro) com o spinner acima do
    // texto, centralizada nos dois eixos — mais fácil/seguro que centralizar
    // o spinner sozinho com posicionamento absoluto fixo, já que o tamanho
    // do videoBox muda (ao vivo janela pequena vs. filme/episódio tela cheia).
    brls::Box* loadingBox = new brls::Box();
    loadingBox->setAxis(brls::Axis::COLUMN);
    loadingBox->setPositionType(brls::PositionType::ABSOLUTE);
    loadingBox->setPositionTop(0);
    loadingBox->setPositionLeft(0);
    loadingBox->setPositionRight(0);
    loadingBox->setPositionBottom(0);
    loadingBox->setAlignItems(brls::AlignItems::CENTER);
    loadingBox->setJustifyContent(brls::JustifyContent::CENTER);

    loadingSpinner = new brls::ProgressSpinner(brls::ProgressSpinnerSize::LARGE);
    loadingSpinner->setMarginBottom(12);
    loadingBox->addView(loadingSpinner);

    loadingLabel = new brls::Label();
    loadingLabel->setFontSize(20);
    loadingLabel->setText("Carregando...");
    loadingLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    loadingBox->addView(loadingLabel);

    videoBox->addView(loadingBox);

    errorLabel = new brls::Label();
    errorLabel->setFontSize(18);
    errorLabel->setPositionType(brls::PositionType::ABSOLUTE);
    errorLabel->setPositionTop(0);
    errorLabel->setPositionLeft(20);
    errorLabel->setPositionRight(20);
    errorLabel->setPositionBottom(0);
    errorLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    errorLabel->setVisibility(brls::Visibility::GONE);
    videoBox->addView(errorLabel);

    // OSD existente do player, agora compartilhado por Live e VOD. Em VOD
    // mantém tempo/barra; ao apertar X, o mesmo overlay expande com Áudio,
    // Legendas e Refresh navegáveis pelo direcional.
    osdBox = new brls::Box();
    osdBox->setAxis(brls::Axis::COLUMN);
    osdBox->setPositionType(brls::PositionType::ABSOLUTE);
    osdBox->setPositionLeft(20);
    osdBox->setPositionRight(20);
    osdBox->setPositionBottom(15);

    settingsHudBox = new brls::Box();
    settingsHudBox->setAxis(brls::Axis::COLUMN);
    settingsHudBox->setPadding(12, 16, 12, 16);
    settingsHudBox->setMarginBottom(10);
    settingsHudBox->setVisibility(brls::Visibility::GONE);

    audioHudLabel = new brls::Label();
    audioHudLabel->setFontSize(18);
    audioHudLabel->setTextColor(nvgRGB(255, 255, 255));
    settingsHudBox->addView(audioHudLabel);

    subtitleHudLabel = new brls::Label();
    subtitleHudLabel->setFontSize(18);
    subtitleHudLabel->setTextColor(nvgRGB(255, 255, 255));
    settingsHudBox->addView(subtitleHudLabel);

    refreshHudLabel = new brls::Label();
    refreshHudLabel->setFontSize(18);
    refreshHudLabel->setTextColor(nvgRGB(255, 255, 255));
    settingsHudBox->addView(refreshHudLabel);

    recoveryHudLabel = new brls::Label();
    recoveryHudLabel->setFontSize(15);
    recoveryHudLabel->setTextColor(nvgRGB(255, 255, 255));
    recoveryHudLabel->setText("X: fechar   ↑↓: escolher   ←→: mudar   A: confirmar");
    settingsHudBox->addView(recoveryHudLabel);
    osdBox->addView(settingsHudBox);

    if (!isLive) {
        timeLabel = new brls::Label();
        timeLabel->setFontSize(16);
        timeLabel->setText("--:-- / --:--");
        timeLabel->setTextColor(nvgRGB(255, 255, 255));
        timeLabel->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
        timeLabel->setMarginBottom(6);
        osdBox->addView(timeLabel);

        brls::Box* progressBarBox = new brls::Box();
        progressBarBox->setHeight(24);
        progressBarBox->setJustifyContent(brls::JustifyContent::CENTER);

        brls::Rectangle* progressBarBg = new brls::Rectangle(nvgRGBA(255, 255, 255, 60));
        progressBarBg->setGrow(1.0f);
        progressBarBg->setHeight(6);
        progressBarBg->setCornerRadius(3);
        progressBarBox->addView(progressBarBg);

        progressBarFill = new brls::Rectangle(nvgRGB(2, 177, 244));
        progressBarFill->setPositionType(brls::PositionType::ABSOLUTE);
        progressBarFill->setPositionTop(9);
        progressBarFill->setPositionLeft(0);
        progressBarFill->setHeight(6);
        progressBarFill->setWidth(0);
        progressBarFill->setCornerRadius(3);
        progressBarBox->addView(progressBarFill);
        progressBarWidth = brls::Application::contentWidth - 40;

        progressBarBox->addGestureRecognizer(new brls::PanGestureRecognizer(
            [this](brls::PanGestureStatus status, brls::Sound*) {
                if (settingsHudOpen || progressBarWidth <= 0) return;
                static float dragStartFraction = 0;
                if (status.state == brls::GestureState::UNSURE) return;
                if (status.state == brls::GestureState::INTERRUPTED || status.state == brls::GestureState::FAILED) {
                    draggingProgressBar = false;
                    return;
                }
                if (status.state == brls::GestureState::START) {
                    draggingProgressBar = true;
                    dragStartFraction = progressBarFill->getWidth() / progressBarWidth;
                    showOsd();
                }
                float delta = status.position.x - status.startPosition.x;
                float fraction = std::min(1.0f, std::max(0.0f, dragStartFraction + delta / progressBarWidth));
                progressBarFill->setWidth(progressBarWidth * fraction);
                if (timeLabel) {
                    double previewSeconds = fraction * lastKnownDuration;
                    timeLabel->setText(formatPlayerTime(previewSeconds) + " / " + formatPlayerTime(lastKnownDuration));
                }
                if (status.state == brls::GestureState::END) {
                    draggingProgressBar = false;
                    mpvView->player()->seekAbsolute(fraction * lastKnownDuration);
                    showOsd();
                }
            },
            brls::PanAxis::HORIZONTAL));
        osdBox->addView(progressBarBox);
    }
    osdBox->setVisibility(brls::Visibility::GONE);
    videoBox->addView(osdBox);

    videoWrapperBox->addView(videoBox);

    rootBox->addView(videoWrapperBox);

    epgBox = new brls::Box();
    epgBox->setAxis(brls::Axis::COLUMN);
    epgBox->setPadding(12, 20, 20, 20);

    brls::Box* epgStatusRow = new brls::Box();
    epgStatusRow->setAxis(brls::Axis::ROW);
    epgStatusRow->setAlignItems(brls::AlignItems::CENTER);

    epgSpinner = new brls::ProgressSpinner();
    epgSpinner->setWidth(18);
    epgSpinner->setHeight(18);
    epgSpinner->setMarginRight(10);
    epgStatusRow->addView(epgSpinner);

    epgStatusLabel = new brls::Label();
    epgStatusLabel->setFontSize(18);
    epgStatusLabel->setText("Carregando programação...");
    epgStatusRow->addView(epgStatusLabel);
    epgBox->addView(epgStatusRow);

    epgListBox = new brls::Box();
    epgListBox->setAxis(brls::Axis::COLUMN);
    epgBox->addView(epgListBox);

    rootBox->addView(epgBox);

    return rootBox;
}

void PlayerActivity::onContentAvailable() {
    applyLayout();

    rootBox->registerAction("Trocar canal/HUD", brls::BUTTON_UP, [this](brls::View*) {
        if (settingsHudOpen) { moveSettingsHudRow(-1); return true; }
        if (!isLive) return false;
        changeChannel(-1);
        return true;
    });
    rootBox->registerAction("Trocar canal/HUD", brls::BUTTON_DOWN, [this](brls::View*) {
        if (settingsHudOpen) { moveSettingsHudRow(1); return true; }
        if (!isLive) return false;
        changeChannel(1);
        return true;
    });
    rootBox->registerAction("Tela cheia/Pausar/HUD", brls::BUTTON_A, [this](brls::View*) {
        if (settingsHudOpen) { activateSettingsHudRow(); return true; }
        if (isLive) {
            if (!fullscreen) toggleFullscreen();
        } else {
            togglePause();
        }
        return true;
    });
    rootBox->registerAction("Retroceder/HUD", brls::BUTTON_LEFT, [this](brls::View*) {
        if (settingsHudOpen) { changeSettingsHudValue(-1); return true; }
        if (isLive) return false;
        seekBy(-10);
        return true;
    });
    rootBox->registerAction("Avançar/HUD", brls::BUTTON_RIGHT, [this](brls::View*) {
        if (settingsHudOpen) { changeSettingsHudValue(1); return true; }
        if (isLive) return false;
        seekBy(10);
        return true;
    });
    rootBox->registerAction("Voltar", brls::BUTTON_B, [this](brls::View*) {
        if (settingsHudOpen) { toggleSettingsHud(); return true; }
        handleBack();
        return true;
    });
    rootBox->registerAction("Áudio/Legendas", brls::BUTTON_X, [this](brls::View*) {
        toggleSettingsHud();
        return true;
    });
    rootBox->registerAction("Favoritar", brls::BUTTON_Y, [this](brls::View*) {
        iptv::toggleFavorite(iptv::toFavoriteItem(item));
        return true;
    });

    promptResumeIfNeeded();
    if (isLive) loadEpg();
}

void PlayerActivity::promptResumeIfNeeded() {
    // Ao vivo não tem "posição" que faça sentido retomar depois.
    if (isLive) {
        playCurrentItem(0);
        return;
    }

    auto progress = iptv::getProgress(item.id);
    // Só oferece continuar se não estiver "quase no fim" (95%) — igual
    // `PlayerView.tsx`, pra não perguntar bobagem bem no final por engano.
    if (!progress || progress->positionSeconds >= progress->durationSeconds * 0.95) {
        playCurrentItem(0);
        return;
    }

    double resumeSeconds = progress->positionSeconds;
    // O dialog não é filho da view da Activity (é um overlay à parte), então
    // sobrevive se o usuário sair da tela (B) antes de escolher um botão —
    // sem o `alive`, o clique chamaria `playCurrentItem` numa Activity já
    // destruída (mesmo motivo de todo outro callback adiado neste arquivo).
    auto aliveFlag = alive;
    // `dialog->open()` chama `Application::pushActivity` — se feito direto
    // daqui, ainda estamos dentro do `onContentAvailable()` desta própria
    // Activity, chamado ANTES dela mesma ser empilhada (ver
    // `Application::pushActivity`: só dá `push_back` na pilha depois de
    // rodar `onContentAvailable()`). Isso fazia o diálogo entrar na pilha
    // primeiro e o player ser empilhado por cima logo em seguida — abrindo
    // "atrás". Adiar um tick garante que o player já esteja na pilha antes.
    brls::delay(1, [this, aliveFlag, resumeSeconds]() {
        if (!*aliveFlag) return;
        brls::Dialog* dialog = new brls::Dialog("Você já assistiu parte disso. Deseja continuar de onde parou?");
        dialog->addButton("Continuar de onde parou", [this, aliveFlag, resumeSeconds]() {
            if (!*aliveFlag) return;
            playCurrentItem(resumeSeconds);
        });
        dialog->addButton("Do início", [this, aliveFlag]() {
            if (!*aliveFlag) return;
            playCurrentItem(0);
        });
        dialog->setCancelable(false);
        dialog->open();
    });
}

void PlayerActivity::playCurrentItem(double startSeconds) {
    playbackStarted = false;
    loadingLabel->setVisibility(brls::Visibility::VISIBLE);
    loadingSpinner->setVisibility(brls::Visibility::VISIBLE);
    errorLabel->setVisibility(brls::Visibility::GONE);

    std::string url = iptv::resolveStreamUrl(auth, item);

    lastPlaybackUrl = url;
    // Carrega SEMPRE do início: pedir pro mpv já abrir o arquivo direto numa
    // posição (opção `start=` do `loadUrl`) falha silenciosamente com
    // alguns provedores — o stream nunca chega a tocar, sem erro nenhum
    // (confirmado com usuário real: a retomada nunca funcionava, sempre
    // caía pro início). Guarda a posição desejada e só faz o seek de
    // verdade quando o vídeo já está tocando (ver `setOnProgress` acima) —
    // mesmo mecanismo, comprovado, do arrastar a barra de progresso.
    pendingResumeSeconds = startSeconds > 0 ? startSeconds : -1;
    mpvView->player()->loadUrl(url, 0);
    mpvView->setReadyToRender(true);
    titleLabel->setText(item.name);
    iptv::recordHistory(iptv::toFavoriteItem(item));
}

void PlayerActivity::loadEpg() {
    if (item.streamId == 0 || auth.mode != iptv::AuthMode::Xtream) {
        epgStatusLabel->setText("Sem informações de programação disponíveis.");
        epgSpinner->setVisibility(brls::Visibility::GONE);
        return;
    }

    epgStatusLabel->setText("Carregando programação...");
    epgSpinner->setVisibility(brls::Visibility::VISIBLE);
    epgListBox->clearViews();

    auto aliveFlag = alive;
    iptv::Credentials creds = auth.credentials;
    long streamId = item.streamId;
    brls::async([this, aliveFlag, creds, streamId]() {
        iptv::EpgResult result = iptv::getShortEpg(creds, streamId, 10);
        brls::sync([this, aliveFlag, result]() {
            if (!*aliveFlag) return;
            if (!result.ok) {
                epgStatusLabel->setText("Sem informações de programação disponíveis.");
                epgSpinner->setVisibility(brls::Visibility::GONE);
                return;
            }
            epg = result.listings;
            populateEpgList();
        });
    });
}

void PlayerActivity::populateEpgList() {
    epgSpinner->setVisibility(brls::Visibility::GONE);
    epgListBox->clearViews();

    if (epg.empty()) {
        epgStatusLabel->setText("Sem informações de programação disponíveis.");
        return;
    }
    epgStatusLabel->setText("Programação");

    for (const auto& listing : epg) {
        brls::Box* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setPaddingTop(4);
        row->setPaddingBottom(4);

        brls::Label* timeLabel = new brls::Label();
        timeLabel->setFontSize(16);
        timeLabel->setWidth(120);
        timeLabel->setText(formatEpgTime(listing.start) + "-" + formatEpgTime(listing.end));
        row->addView(timeLabel);

        brls::Label* titleRowLabel = new brls::Label();
        titleRowLabel->setFontSize(16);
        titleRowLabel->setGrow(1.0f);
        titleRowLabel->setText(listing.title);
        row->addView(titleRowLabel);

        epgListBox->addView(row);
    }
}

void PlayerActivity::changeChannel(int step) {
    if (liveChannels.empty()) return;
    auto it = std::find_if(liveChannels.begin(), liveChannels.end(),
                            [this](const iptv::CatalogItem& c) { return c.id == item.id; });
    if (it == liveChannels.end()) return;

    size_t idx = static_cast<size_t>(it - liveChannels.begin());
    size_t count = liveChannels.size();
    size_t nextIdx = (idx + static_cast<size_t>(step + static_cast<int>(count))) % count;

    item = liveChannels[nextIdx];
    playCurrentItem(0);
    loadEpg();
}

void PlayerActivity::toggleFullscreen() {
    fullscreen = !fullscreen;
    applyLayout();
}

void PlayerActivity::applyLayout() {
    if (fullscreen) {
        headerBox->setVisibility(brls::Visibility::GONE);
        epgBox->setVisibility(brls::Visibility::GONE);
        videoBox->setWidth(brls::Application::contentWidth);
        videoBox->setHeight(brls::Application::contentHeight);
    } else {
        headerBox->setVisibility(brls::Visibility::VISIBLE);
        epgBox->setVisibility(isLive ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
        videoBox->setWidth(960);
        videoBox->setHeight(540);
    }
}

void PlayerActivity::togglePause() {
    mpvView->player()->setPaused(!mpvView->player()->isPaused());
    showOsd();
}

void PlayerActivity::seekBy(double deltaSeconds) {
    mpvView->player()->seek(deltaSeconds);
    showOsd();
}

void PlayerActivity::showOsd() {
    if (!osdBox) return;
    osdBox->setVisibility(brls::Visibility::VISIBLE);

    if (osdHideToken != 0) {
        brls::cancelDelay(osdHideToken);
        osdHideToken = 0;
    }
    // Com o menu expandido, ou pausado, o OSD fica na tela.
    if (settingsHudOpen || mpvView->player()->isPaused()) return;

    auto aliveFlag = alive;
    osdHideToken = brls::delay(4000, [this, aliveFlag]() {
        if (!*aliveFlag) return;
        osdHideToken = 0;
        if (osdBox) osdBox->setVisibility(brls::Visibility::GONE);
    });
}


std::string PlayerActivity::friendlyTrackName(const iptv::MpvTrackInfo& track, const std::string& fallback) const {
    std::string language = track.language;
    std::transform(language.begin(), language.end(), language.begin(), [](unsigned char c) { return std::tolower(c); });
    std::string langName;
    if (language == "pt" || language == "por" || language == "pt-pt") langName = "Português";
    else if (language == "pt-br" || language == "pob") langName = "Português BR";
    else if (language == "en" || language == "eng") langName = "English";
    else if (language == "es" || language == "spa") langName = "Español";
    else if (language == "fr" || language == "fra" || language == "fre") langName = "Français";
    else if (!track.language.empty()) langName = track.language;

    if (!track.title.empty()) {
        if (!langName.empty() && track.title.find(langName) == std::string::npos) return langName + " — " + track.title;
        return track.title;
    }
    if (!langName.empty()) return langName;
    return fallback + " " + std::to_string(track.id);
}

void PlayerActivity::refreshHudTracks() {
    audioTrackIds.clear();
    audioTrackNames.clear();
    subtitleTrackIds.clear();
    subtitleTrackNames.clear();
    subtitleTrackIds.push_back(0);
    subtitleTrackNames.push_back("Desligadas");
    audioTrackIndex = 0;
    subtitleTrackIndex = 0;

    auto tracks = mpvView->player()->getTracks();
    for (const auto& track : tracks) {
        if (track.type == "audio") {
            audioTrackIds.push_back(track.id);
            audioTrackNames.push_back(friendlyTrackName(track, "Áudio"));
            if (track.selected) audioTrackIndex = static_cast<int>(audioTrackIds.size()) - 1;
        } else if (track.type == "sub") {
            subtitleTrackIds.push_back(track.id);
            subtitleTrackNames.push_back(friendlyTrackName(track, "Legenda"));
            if (track.selected) subtitleTrackIndex = static_cast<int>(subtitleTrackIds.size()) - 1;
        }
    }
    updateSettingsHud();
}

void PlayerActivity::updateSettingsHud() {
    if (!audioHudLabel || !subtitleHudLabel || !refreshHudLabel) return;
    const std::string audioName = audioTrackNames.empty() ? "Sem faixas" : audioTrackNames[audioTrackIndex];
    const std::string subtitleName = subtitleTrackNames.empty() ? "Desligadas" : subtitleTrackNames[subtitleTrackIndex];
    audioHudLabel->setText(std::string(settingsHudRow == 0 ? "▶ " : "  ") + "Áudio       ◀ " + audioName + " ▶");
    subtitleHudLabel->setText(std::string(settingsHudRow == 1 ? "▶ " : "  ") + "Legendas    ◀ " + subtitleName + " ▶");
    refreshHudLabel->setText(std::string(settingsHudRow == 2 ? "▶ " : "  ") + "Refresh       Recarregar stream");
}

void PlayerActivity::toggleSettingsHud() {
    if (!settingsHudBox || !osdBox) return;
    settingsHudOpen = !settingsHudOpen;
    if (settingsHudOpen) {
        settingsHudRow = 0;
        refreshHudTracks();
        settingsHudBox->setVisibility(brls::Visibility::VISIBLE);
        showOsd();
    } else {
        settingsHudBox->setVisibility(brls::Visibility::GONE);
        showOsd();
    }
}

void PlayerActivity::moveSettingsHudRow(int delta) {
    settingsHudRow = (settingsHudRow + delta + 3) % 3;
    updateSettingsHud();
    showOsd();
}

void PlayerActivity::changeSettingsHudValue(int delta) {
    if (settingsHudRow == 0 && !audioTrackIds.empty()) {
        const int count = static_cast<int>(audioTrackIds.size());
        audioTrackIndex = (audioTrackIndex + delta + count) % count;
        mpvView->player()->selectAudioTrack(audioTrackIds[audioTrackIndex]);
    } else if (settingsHudRow == 1 && !subtitleTrackIds.empty()) {
        const int count = static_cast<int>(subtitleTrackIds.size());
        subtitleTrackIndex = (subtitleTrackIndex + delta + count) % count;
        mpvView->player()->selectSubtitleTrack(subtitleTrackIds[subtitleTrackIndex]);
    }
    updateSettingsHud();
    showOsd();
}

void PlayerActivity::activateSettingsHudRow() {
    if (settingsHudRow == 0 && !audioTrackIds.empty()) {
        mpvView->player()->selectAudioTrack(audioTrackIds[audioTrackIndex]);
    } else if (settingsHudRow == 1 && !subtitleTrackIds.empty()) {
        mpvView->player()->selectSubtitleTrack(subtitleTrackIds[subtitleTrackIndex]);
    } else if (settingsHudRow == 2) {
        refreshStream(false);
    }
    updateSettingsHud();
}

void PlayerActivity::refreshStream(bool automatic) {
    if (lastPlaybackUrl.empty()) return;
    const double resume = isLive ? 0 : mpvView->player()->getPositionSeconds();
    playbackStarted = false;
    pendingResumeSeconds = resume > 0 ? resume : -1;
    loadingLabel->setText(automatic ? "A recuperar stream..." : "A recarregar stream...");
    loadingLabel->setVisibility(brls::Visibility::VISIBLE);
    loadingSpinner->setVisibility(brls::Visibility::VISIBLE);
    errorLabel->setVisibility(brls::Visibility::GONE);
    mpvView->player()->loadUrl(lastPlaybackUrl, 0);
    mpvView->setReadyToRender(true);
}

void PlayerActivity::armRecoveryWatch() {
    if (recoveryToken != 0) brls::cancelDelay(recoveryToken);
    auto aliveFlag = alive;
    recoveryToken = brls::delay(1500, [this, aliveFlag]() {
        if (!*aliveFlag) return;
        recoveryToken = 0;
        if (!playbackStarted || mpvView->player()->isPaused()) {
            armRecoveryWatch();
            return;
        }
        if (!mpvView->player()->isBuffering()) {
            recoveryPending = false;
            armRecoveryWatch();
            return;
        }
        if (!recoveryPending) {
            recoveryPending = true;
            auto aliveAgain = alive;
            recoveryToken = brls::delay(2500, [this, aliveAgain]() {
                if (!*aliveAgain) return;
                recoveryToken = 0;
                if (playbackStarted && !mpvView->player()->isPaused() && mpvView->player()->isBuffering()) {
                    refreshStream(true);
                }
                recoveryPending = false;
                armRecoveryWatch();
            });
            return;
        }
        armRecoveryWatch();
    });
}

void PlayerActivity::updateTimeLabel(double position, double duration) {
    if (!timeLabel) return;
    lastKnownDuration = duration;

    // Arrastando, o preview do próprio gesto já está atualizando o texto e
    // a barra (ver progressBarBox->addGestureRecognizer) — sobrescrever
    // aqui com a posição real (ainda a antiga, o seek só acontece ao
    // soltar) faria o preview "voltar" visualmente no meio do arrasto.
    if (draggingProgressBar) return;

    timeLabel->setText(formatPlayerTime(position) + " / " + formatPlayerTime(duration));

    if (progressBarFill && duration > 0) {
        float ratio = static_cast<float>(std::min(1.0, std::max(0.0, position / duration)));
        progressBarFill->setWidth(progressBarWidth * ratio);
    }
}

void PlayerActivity::handleBack() {
    if (isLive && fullscreen) {
        fullscreen = false;
        applyLayout();
        return;
    }
    brls::Application::popActivity();
}
