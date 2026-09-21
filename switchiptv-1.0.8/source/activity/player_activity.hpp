#pragma once

#include <borealis.hpp>
#include <memory>
#include <vector>

#include "data/catalog_types.hpp"
#include "data/storage.hpp"
#include "data/xtream_client.hpp"
#include "player/mpv_player.hpp"

class MpvView;

namespace brls {
class Rectangle;
class Label;
}

// Player — porte de `PlayerView.tsx`. Canal ao vivo abre pequeno com a
// programação completa embaixo (Cima/Baixo trocam de canal, A alterna tela
// cheia); filme/episódio vai direto pra tela cheia. Mesmo comportamento já
// portado 3x (Android, web, Roku).
class PlayerActivity : public brls::Activity {
  public:
    PlayerActivity(iptv::StoredAuth auth, iptv::CatalogItem item, std::vector<iptv::CatalogItem> liveChannels);
    ~PlayerActivity() override;

    brls::View* createContentView() override;
    void onContentAvailable() override;

  private:
    iptv::StoredAuth auth;
    iptv::CatalogItem item;
    std::vector<iptv::CatalogItem> liveChannels;
    bool isLive;
    bool fullscreen;
    std::vector<iptv::EpgListing> epg;

    // Ver `catalog_content_view.cpp` — mesmo motivo: Activity não tem o
    // mecanismo de deletionToken de View, então usamos um flag manual pra
    // não tocar `this` se o usuário sair da tela antes da EPG responder.
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    brls::Box* rootBox = nullptr;
    brls::Box* headerBox = nullptr;
    brls::Label* titleLabel = nullptr;
    brls::Label* hintLabel = nullptr;
    brls::Label* timeLabel = nullptr;
    brls::Rectangle* progressBarFill = nullptr;
    float progressBarWidth = 0;
    // Suprime a atualização automática (vinda de `setOnProgress`) enquanto o
    // dedo está arrastando a barra — senão a posição real de reprodução
    // ficaria "brigando" visualmente com o preview do arrasto.
    bool draggingProgressBar = false;
    double lastKnownDuration = 0;
    // OSD de reprodução (só VOD/episódio): barra de progresso + tempo
    // sobrepostos na parte de baixo do vídeo, estilo apps de streaming.
    // Some sozinho após alguns segundos e volta com seek/pausa.
    brls::Box* osdBox = nullptr;
    brls::Box* settingsHudBox = nullptr;
    brls::Label* audioHudLabel = nullptr;
    brls::Label* subtitleHudLabel = nullptr;
    brls::Label* refreshHudLabel = nullptr;
    brls::Label* recoveryHudLabel = nullptr;
    bool settingsHudOpen = false;
    int settingsHudRow = 0;
    std::vector<int> audioTrackIds;
    std::vector<std::string> audioTrackNames;
    int audioTrackIndex = 0;
    std::vector<int> subtitleTrackIds;
    std::vector<std::string> subtitleTrackNames;
    int subtitleTrackIndex = 0;
    bool recoveryPending = false;
    size_t recoveryToken = 0;
    size_t osdHideToken = 0;
    brls::Box* videoBox = nullptr;
    MpvView* mpvView = nullptr;
    brls::Label* loadingLabel = nullptr;
    brls::ProgressSpinner* loadingSpinner = nullptr;
    brls::Label* errorLabel = nullptr;
    bool playbackStarted = false;
    std::string lastPlaybackUrl;
    // >0 enquanto uma retomada está pendente — só aplicada (via seek, não
    // via abrir o arquivo já numa posição) quando o vídeo realmente começa
    // a tocar. Ver `playCurrentItem`/`setOnProgress` pro porquê.
    double pendingResumeSeconds = -1;
    brls::Box* epgBox = nullptr;
    brls::Label* epgStatusLabel = nullptr;
    brls::ProgressSpinner* epgSpinner = nullptr;
    brls::Box* epgListBox = nullptr;

    void promptResumeIfNeeded();
    void playCurrentItem(double startSeconds);
    void loadEpg();
    void populateEpgList();
    void changeChannel(int step);
    void toggleFullscreen();
    void togglePause();
    void seekBy(double deltaSeconds);
    void applyLayout();
    void handleBack();
    void updateTimeLabel(double position, double duration);
    // Mostra o OSD; agenda o auto-esconder (4s) a menos que esteja pausado —
    // pausado, fica visível até despausar.
    void showOsd();
    void toggleSettingsHud();
    void refreshHudTracks();
    void updateSettingsHud();
    void moveSettingsHudRow(int delta);
    void changeSettingsHudValue(int delta);
    void activateSettingsHudRow();
    void refreshStream(bool automatic = false);
    void armRecoveryWatch();
    std::string friendlyTrackName(const iptv::MpvTrackInfo& track, const std::string& fallback) const;
};
