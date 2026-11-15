/* FH2 1.0.0.64: the developer badge. On a name tag over a car (UI.zip Scenes/Anthem/AR_OpponentLabel.xaml, one page per car) FH2MP's developers
 * have FH2MP's logo after their gamertag, the way the later Horizon games mark their studio's players: a white border with round corners and the
 * slanted F inside it (the logo's own outline and F, the F at 85%), about as tall as the gamertag's capital letters.
 *
 * No game code is hooked. The tag's page gets a Control behind the gamertag (ui_dev_badge in fh2mp_mod.c: one exact replacement, handed to the
 * platform's set_file), with its template written in place as the game's own pages do. The badge is collapsed, and one MultiDataTrigger per
 * developer shows it when the tag's UserProfile.FormattedGamertag is that name and the tag's IsDrivatar is False: a Drivatar carries a real
 * player's name and must not wear the badge. Gamertags are unique on a server after look-alike folding (auth.FoldTag), so a name can't be
 * borrowed. The logo is drawn as FZMaterial polygons (the page ships no picture): the border is two C-shaped halves that overlap by 2 units half
 * way along the top and bottom edges, so no line shows between them.
 *
 * The names are DEV_BADGE_NAMES: letters, digits and spaces only, because they go into the page as written (tests/test_dev_badge.sh checks).
 * Only one mod can replace a game file: if another mod has replaced the name tag page, the platform refuses this one, the shim's log says so and
 * the tags stay as that mod made them. FH2MP_NO_DEVBADGE=1 turns the badge off.
 *
 * Seen in the game (offline, 2026-11-15, on Drivatars' tags, with this markup): the shapes, the place after the name, tags without the badge
 * unchanged, and no badge on a Drivatar that has a listed name. Not seen: a real player's tag online, and that IsDrivatar is False there. */
#define DEV_BADGE_NAMES(X) X("Charelene") X("AVALON")

static const char DEV_BADGE_OLD[] =
    "                                <Grid Margin=\"50\">\r\n"
    "                                    <local:FZTextBlock Style=\"{StaticResource A_Style_OpponentTag}\"\r\n"
    "                                                       Text=\"{Binding UserProfile.FormattedGamertag}\" />\r\n"
    "                                </Grid>\r\n";
#define DEV_BADGE_TRIGGER(name) \
    "                                                <MultiDataTrigger>\r\n" \
    "                                                    <MultiDataTrigger.Conditions>\r\n" \
    "                                                        <Condition Binding=\"{Binding UserProfile.FormattedGamertag}\"\r\n" \
    "                                                                   Value=\"" name "\" />\r\n" \
    "                                                        <Condition Binding=\"{Binding IsDrivatar}\"\r\n" \
    "                                                                   Value=\"False\" />\r\n" \
    "                                                    </MultiDataTrigger.Conditions>\r\n" \
    "                                                    <Setter TargetName=\"FH2MP_DevBadge\"\r\n" \
    "                                                            Property=\"Visibility\"\r\n" \
    "                                                            Value=\"Visible\" />\r\n" \
    "                                                </MultiDataTrigger>\r\n"
static const char DEV_BADGE_NEW[] =
    "                                <Grid Margin=\"50\">\r\n"
    "                                    <local:FZTextBlock Style=\"{StaticResource A_Style_OpponentTag}\"\r\n"
    "                                                       Text=\"{Binding UserProfile.FormattedGamertag}\" />\r\n"
    "                                </Grid>\r\n"
    "                                <Control VerticalAlignment=\"Center\">\r\n"
    "                                    <Control.Template>\r\n"
    "                                        <ControlTemplate TargetType=\"{x:Type Control}\">\r\n"
    "                                            <Grid x:Name=\"FH2MP_DevBadge\"\r\n"
    "                                                  Width=\"180\"\r\n"
    "                                                  Height=\"180\"\r\n"
    "                                                  Margin=\"0,0,50,0\"\r\n"
    "                                                  VerticalAlignment=\"Center\"\r\n"
    "                                                  Visibility=\"Collapsed\">\r\n"
    "                                                <local:FZMaterial Color=\"#FFFFFFFF\"\r\n"
    "                                                                  Width=\"92\"\r\n"
    "                                                                  Height=\"180\"\r\n"
    "                                                                  HorizontalAlignment=\"Left\"\r\n"
    "                                                                  VerticalAlignment=\"Top\"\r\n"
    "                                                                  Margin=\"0,0,0,0\">\r\n"
    "                                                    <local:FZMaterial.Data>\r\n"
    "                                                        <local:FZGeometry Data=\"M92,180 38.79,180 28.75,178.68 19.4,174.8 11.36,168.64 5.2,160.6 1.32,151.25 0,141.21 0,38.79 1.32,28.75 5.2,19.4 11.36,11.36 19.4,5.2 28.75,1.32 38.79,0 92,0 92,13.97 38.79,13.97 32.37,14.81 26.38,17.29 21.24,21.24 17.29,26.38 14.81,32.37 13.97,38.79 13.97,141.21 14.81,147.63 17.29,153.62 21.24,158.76 26.38,162.71 32.37,165.19 38.79,166.03 92,166.03z\" />\r\n"
    "                                                    </local:FZMaterial.Data>\r\n"
    "                                                </local:FZMaterial>\r\n"
    "                                                <local:FZMaterial Color=\"#FFFFFFFF\"\r\n"
    "                                                                  Width=\"90\"\r\n"
    "                                                                  Height=\"180\"\r\n"
    "                                                                  HorizontalAlignment=\"Left\"\r\n"
    "                                                                  VerticalAlignment=\"Top\"\r\n"
    "                                                                  Margin=\"90,0,0,0\">\r\n"
    "                                                    <local:FZMaterial.Data>\r\n"
    "                                                        <local:FZGeometry Data=\"M0,0 51.21,0 61.25,1.32 70.6,5.2 78.64,11.36 84.8,19.4 88.68,28.75 90,38.79 90,141.21 88.68,151.25 84.8,160.6 78.64,168.64 70.6,174.8 61.25,178.68 51.21,180 0,180 0,166.03 51.21,166.03 57.63,165.19 63.62,162.71 68.76,158.76 72.71,153.62 75.19,147.63 76.03,141.21 76.03,38.79 75.19,32.37 72.71,26.38 68.76,21.24 63.62,17.29 57.63,14.81 51.21,13.97 0,13.97z\" />\r\n"
    "                                                    </local:FZMaterial.Data>\r\n"
    "                                                </local:FZMaterial>\r\n"
    "                                                <local:FZMaterial Color=\"#FFFFFFFF\"\r\n"
    "                                                                  Width=\"81.56\"\r\n"
    "                                                                  Height=\"98.92\"\r\n"
    "                                                                  HorizontalAlignment=\"Left\"\r\n"
    "                                                                  VerticalAlignment=\"Top\"\r\n"
    "                                                                  Margin=\"49,40.54,0,0\">\r\n"
    "                                                    <local:FZMaterial.Data>\r\n"
    "                                                        <local:FZGeometry Data=\"M17.26,0 81.56,0 78.34,18.47 37.12,18.47 33.09,41.55 70.35,41.55 67.48,58.03 30.22,58.03 23.08,98.92 0,98.92z\" />\r\n"
    "                                                    </local:FZMaterial.Data>\r\n"
    "                                                </local:FZMaterial>\r\n"
    "                                            </Grid>\r\n"
    "                                            <ControlTemplate.Triggers>\r\n"
    DEV_BADGE_NAMES(DEV_BADGE_TRIGGER)
    "                                            </ControlTemplate.Triggers>\r\n"
    "                                        </ControlTemplate>\r\n"
    "                                    </Control.Template>\r\n"
    "                                </Control>\r\n";
