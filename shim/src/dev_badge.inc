/* FH2 1.0.0.64: the developer badge. On a name tag over a car (UI.zip Scenes/Anthem/AR_OpponentLabel.xaml, one page per car) FH2MP's developers
 * have the Encore logo after their gamertag, the way the later Horizon games mark their studio's players: a white border with round corners and the
 * slanted E inside it (the logo's own outline and E, the E at 85%), about as tall as the gamertag's capital letters. The same badge follows their
 * gamertag in the game's player lists (Controls/PlayerListControl.xaml: the online Players tab, the starting grid, an event's results) and on the
 * Rivals, speed trap and XP leaderboards (one driver name cell in each page, word for word the same); see "In the lists" below.
 *
 * No game code is hooked. In the tag's page the gamertag's Grid becomes a Control (ui_dev_badge in fh2mp_mod.c: one exact replacement, handed to
 * the platform's set_file) whose template, written in place as the game's own pages do, holds that same gamertag text and the badge behind it,
 * collapsed. The logo is drawn as FZMaterial polygons (the page ships no picture): the border is two C-shaped halves that overlap by 2 units half
 * way along the top and bottom edges, so no line shows between them.
 *
 * The tag has only the FORMATTED gamertag (UserProfile.FormattedGamertag; a plain one reads empty there), and for a club's leader or mod that is
 * the name plus a mark, a character of the game's Anthem font drawn as a shield: U+222B the leader's star shield, U+222C the mod's plain one. So
 * a developer's tag can read three ways, and each has a rule (a MultiDataTrigger): the text is that, and the tag's IsDrivatar is False (a Drivatar
 * carries a real player's name and must not wear the badge; a real player's tag does read False). The rule shows the badge, and for the two texts
 * with a mark it also sets the text to the plain name: the badge takes the shield's place. Gamertags are unique on a server after look-alike
 * folding (auth.FoldTag), so a name can't be borrowed.
 *
 * The names are DEV_BADGE_NAMES: letters, digits and spaces only, because they go into the page as written (tests/test_dev_badge.sh checks).
 * Only one mod can replace a game file: if another mod has replaced the name tag page, the platform refuses this one, the shim's log says so and
 * the tags stay as that mod made them. FH2MP_NO_DEVBADGE=1 turns the badge off.
 *
 * Seen in the game, 2026-11-15. Offline, on Drivatars' tags: the shapes, the place after the name, tags without the badge unchanged, no badge on
 * a Drivatar that has a listed name. Online: a player's tag reads as not a Drivatar, and a club leader's gamertag is the name plus U+222B with no
 * space; with these rules her tag shows the plain name and then the badge. Not seen: a club mod's mark (taken to be U+222C, the leader mark's
 * near-twin in the font).
 *
 * In the lists the gamertag's text line becomes the same kind of Control: the text (its style kept) and the badge, scaled down by a Viewbox to
 * the list's 24-unit type. A list row has no IsDrivatar, so the rules there are on the gamertag alone (DataTriggers, three per name as above):
 * offline, a Drivatar that carries a developer's name gets the badge in a race's grid and results. The player list's text was named PlayerName;
 * nothing refers to that name (not the page, and the exe only has PlayerName as a property of the online event pages), so it is renamed. */
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
    "                                                </MultiDataTrigger>\r\n" \
    "                                                <MultiDataTrigger>\r\n" \
    "                                                    <MultiDataTrigger.Conditions>\r\n" \
    "                                                        <Condition Binding=\"{Binding UserProfile.FormattedGamertag}\"\r\n" \
    "                                                                   Value=\"" name "&#x222b;\" />\r\n" \
    "                                                        <Condition Binding=\"{Binding IsDrivatar}\"\r\n" \
    "                                                                   Value=\"False\" />\r\n" \
    "                                                    </MultiDataTrigger.Conditions>\r\n" \
    "                                                    <Setter TargetName=\"FH2MP_DevName\"\r\n" \
    "                                                            Property=\"Text\"\r\n" \
    "                                                            Value=\"" name "\" />\r\n" \
    "                                                    <Setter TargetName=\"FH2MP_DevBadge\"\r\n" \
    "                                                            Property=\"Visibility\"\r\n" \
    "                                                            Value=\"Visible\" />\r\n" \
    "                                                </MultiDataTrigger>\r\n" \
    "                                                <MultiDataTrigger>\r\n" \
    "                                                    <MultiDataTrigger.Conditions>\r\n" \
    "                                                        <Condition Binding=\"{Binding UserProfile.FormattedGamertag}\"\r\n" \
    "                                                                   Value=\"" name "&#x222c;\" />\r\n" \
    "                                                        <Condition Binding=\"{Binding IsDrivatar}\"\r\n" \
    "                                                                   Value=\"False\" />\r\n" \
    "                                                    </MultiDataTrigger.Conditions>\r\n" \
    "                                                    <Setter TargetName=\"FH2MP_DevName\"\r\n" \
    "                                                            Property=\"Text\"\r\n" \
    "                                                            Value=\"" name "\" />\r\n" \
    "                                                    <Setter TargetName=\"FH2MP_DevBadge\"\r\n" \
    "                                                            Property=\"Visibility\"\r\n" \
    "                                                            Value=\"Visible\" />\r\n" \
    "                                                </MultiDataTrigger>\r\n"
static const char DEV_BADGE_NEW[] =
    "                                <Control VerticalAlignment=\"Center\">\r\n"
    "                                    <Control.Template>\r\n"
    "                                        <ControlTemplate TargetType=\"{x:Type Control}\">\r\n"
    "                                            <StackPanel Orientation=\"Horizontal\">\r\n"
    "                                                <Grid Margin=\"50\">\r\n"
    "                                                    <local:FZTextBlock x:Name=\"FH2MP_DevName\"\r\n"
    "                                                                       Style=\"{StaticResource A_Style_OpponentTag}\"\r\n"
    "                                                                       Text=\"{Binding UserProfile.FormattedGamertag}\" />\r\n"
    "                                                </Grid>\r\n"
    "                                                <Grid x:Name=\"FH2MP_DevBadge\"\r\n"
    "                                                      Width=\"180\"\r\n"
    "                                                      Height=\"180\"\r\n"
    "                                                      Margin=\"0,0,50,0\"\r\n"
    "                                                      VerticalAlignment=\"Center\"\r\n"
    "                                                      Visibility=\"Collapsed\">\r\n"
    "                                                    <local:FZMaterial Color=\"#FFFFFFFF\"\r\n"
    "                                                                      Width=\"92\"\r\n"
    "                                                                      Height=\"180\"\r\n"
    "                                                                      HorizontalAlignment=\"Left\"\r\n"
    "                                                                      VerticalAlignment=\"Top\"\r\n"
    "                                                                      Margin=\"0,0,0,0\">\r\n"
    "                                                        <local:FZMaterial.Data>\r\n"
    "                                                            <local:FZGeometry Data=\"M92,180 38.79,180 28.75,178.68 19.4,174.8 11.36,168.64 5.2,160.6 1.32,151.25 0,141.21 0,38.79 1.32,28.75 5.2,19.4 11.36,11.36 19.4,5.2 28.75,1.32 38.79,0 92,0 92,13.97 38.79,13.97 32.37,14.81 26.38,17.29 21.24,21.24 17.29,26.38 14.81,32.37 13.97,38.79 13.97,141.21 14.81,147.63 17.29,153.62 21.24,158.76 26.38,162.71 32.37,165.19 38.79,166.03 92,166.03z\" />\r\n"
    "                                                        </local:FZMaterial.Data>\r\n"
    "                                                    </local:FZMaterial>\r\n"
    "                                                    <local:FZMaterial Color=\"#FFFFFFFF\"\r\n"
    "                                                                      Width=\"90\"\r\n"
    "                                                                      Height=\"180\"\r\n"
    "                                                                      HorizontalAlignment=\"Left\"\r\n"
    "                                                                      VerticalAlignment=\"Top\"\r\n"
    "                                                                      Margin=\"90,0,0,0\">\r\n"
    "                                                        <local:FZMaterial.Data>\r\n"
    "                                                            <local:FZGeometry Data=\"M0,0 51.21,0 61.25,1.32 70.6,5.2 78.64,11.36 84.8,19.4 88.68,28.75 90,38.79 90,141.21 88.68,151.25 84.8,160.6 78.64,168.64 70.6,174.8 61.25,178.68 51.21,180 0,180 0,166.03 51.21,166.03 57.63,165.19 63.62,162.71 68.76,158.76 72.71,153.62 75.19,147.63 76.03,141.21 76.03,38.79 75.19,32.37 72.71,26.38 68.76,21.24 63.62,17.29 57.63,14.81 51.21,13.97 0,13.97z\" />\r\n"
    "                                                        </local:FZMaterial.Data>\r\n"
    "                                                    </local:FZMaterial>\r\n"
    "                                                    <local:FZMaterial Color=\"#FFFFFFFF\"\r\n"
    "                                                                      Width=\"83.5\"\r\n"
    "                                                                      Height=\"98.92\"\r\n"
    "                                                                      HorizontalAlignment=\"Left\"\r\n"
    "                                                                      VerticalAlignment=\"Top\"\r\n"
    "                                                                      Margin=\"48.25,40.54,0,0\">\r\n"
    "                                                        <local:FZMaterial.Data>\r\n"
    "                                                            <local:FZGeometry Data=\"M17.12,0 83.5,0 80.31,18.41 37.23,18.41 33.6,39.27 69.91,39.27 66.85,57 30.52,57 26.42,80.58 69.37,80.58 66.17,98.92 0,98.92z\" />\r\n"
    "                                                        </local:FZMaterial.Data>\r\n"
    "                                                    </local:FZMaterial>\r\n"
    "                                                </Grid>\r\n"
    "                                            </StackPanel>\r\n"
    "                                            <ControlTemplate.Triggers>\r\n"
    DEV_BADGE_NAMES(DEV_BADGE_TRIGGER)
    "                                            </ControlTemplate.Triggers>\r\n"
    "                                        </ControlTemplate>\r\n"
    "                                    </Control.Template>\r\n"
    "                                </Control>\r\n";

/* In the lists (see above). DEV_BADGE_LIST_OLD occurs once in Controls/PlayerListControl.xaml, DEV_BADGE_BOARD_OLD once in each of
 * Scenes/Anthem/A_RivalsLeaderboard.xaml, A_SpeedTrapLeaderboard.xaml and A_GlobalXPLeaderboard.xaml. */
#define DEV_BADGE_ROW_LOGO \
    "                                                        <local:FZMaterial Color=\"#FFFFFFFF\"\r\n" \
    "                                                                          Width=\"92\"\r\n" \
    "                                                                          Height=\"180\"\r\n" \
    "                                                                          HorizontalAlignment=\"Left\"\r\n" \
    "                                                                          VerticalAlignment=\"Top\"\r\n" \
    "                                                                          Margin=\"0,0,0,0\">\r\n" \
    "                                                            <local:FZMaterial.Data>\r\n" \
    "                                                                <local:FZGeometry Data=\"M92,180 38.79,180 28.75,178.68 19.4,174.8 11.36,168.64 5.2,160.6 1.32,151.25 0,141.21 0,38.79 1.32,28.75 5.2,19.4 11.36,11.36 19.4,5.2 28.75,1.32 38.79,0 92,0 92,13.97 38.79,13.97 32.37,14.81 26.38,17.29 21.24,21.24 17.29,26.38 14.81,32.37 13.97,38.79 13.97,141.21 14.81,147.63 17.29,153.62 21.24,158.76 26.38,162.71 32.37,165.19 38.79,166.03 92,166.03z\" />\r\n" \
    "                                                            </local:FZMaterial.Data>\r\n" \
    "                                                        </local:FZMaterial>\r\n" \
    "                                                        <local:FZMaterial Color=\"#FFFFFFFF\"\r\n" \
    "                                                                          Width=\"90\"\r\n" \
    "                                                                          Height=\"180\"\r\n" \
    "                                                                          HorizontalAlignment=\"Left\"\r\n" \
    "                                                                          VerticalAlignment=\"Top\"\r\n" \
    "                                                                          Margin=\"90,0,0,0\">\r\n" \
    "                                                            <local:FZMaterial.Data>\r\n" \
    "                                                                <local:FZGeometry Data=\"M0,0 51.21,0 61.25,1.32 70.6,5.2 78.64,11.36 84.8,19.4 88.68,28.75 90,38.79 90,141.21 88.68,151.25 84.8,160.6 78.64,168.64 70.6,174.8 61.25,178.68 51.21,180 0,180 0,166.03 51.21,166.03 57.63,165.19 63.62,162.71 68.76,158.76 72.71,153.62 75.19,147.63 76.03,141.21 76.03,38.79 75.19,32.37 72.71,26.38 68.76,21.24 63.62,17.29 57.63,14.81 51.21,13.97 0,13.97z\" />\r\n" \
    "                                                            </local:FZMaterial.Data>\r\n" \
    "                                                        </local:FZMaterial>\r\n" \
    "                                                        <local:FZMaterial Color=\"#FFFFFFFF\"\r\n" \
    "                                                                          Width=\"83.5\"\r\n" \
    "                                                                          Height=\"98.92\"\r\n" \
    "                                                                          HorizontalAlignment=\"Left\"\r\n" \
    "                                                                          VerticalAlignment=\"Top\"\r\n" \
    "                                                                          Margin=\"48.25,40.54,0,0\">\r\n" \
    "                                                            <local:FZMaterial.Data>\r\n" \
    "                                                                <local:FZGeometry Data=\"M17.12,0 83.5,0 80.31,18.41 37.23,18.41 33.6,39.27 69.91,39.27 66.85,57 30.52,57 26.42,80.58 69.37,80.58 66.17,98.92 0,98.92z\" />\r\n" \
    "                                                            </local:FZMaterial.Data>\r\n" \
    "                                                        </local:FZMaterial>\r\n"
#define DEV_BADGE_ROW_HEAD_B \
    "                                                    </Grid>\r\n" \
    "                                                </Viewbox>\r\n" \
    "                                            </StackPanel>\r\n" \
    "                                            <ControlTemplate.Triggers>\r\n"
#define DEV_BADGE_ROW_TRIGGER(name) \
    "                                                <DataTrigger Binding=\"{Binding UserProfile.FormattedGamertag}\" Value=\"" name "\">\r\n" \
    "                                                    <Setter TargetName=\"FH2MP_DevBadge\" Property=\"Visibility\" Value=\"Visible\" />\r\n" \
    "                                                </DataTrigger>\r\n" \
    "                                                <DataTrigger Binding=\"{Binding UserProfile.FormattedGamertag}\" Value=\"" name "&#x222b;\">\r\n" \
    "                                                    <Setter TargetName=\"FH2MP_DevName\" Property=\"Text\" Value=\"" name "\" />\r\n" \
    "                                                    <Setter TargetName=\"FH2MP_DevBadge\" Property=\"Visibility\" Value=\"Visible\" />\r\n" \
    "                                                </DataTrigger>\r\n" \
    "                                                <DataTrigger Binding=\"{Binding UserProfile.FormattedGamertag}\" Value=\"" name "&#x222c;\">\r\n" \
    "                                                    <Setter TargetName=\"FH2MP_DevName\" Property=\"Text\" Value=\"" name "\" />\r\n" \
    "                                                    <Setter TargetName=\"FH2MP_DevBadge\" Property=\"Visibility\" Value=\"Visible\" />\r\n" \
    "                                                </DataTrigger>\r\n"
#define DEV_BADGE_ROW_TAIL \
    "                                            </ControlTemplate.Triggers>\r\n" \
    "                                        </ControlTemplate>\r\n" \
    "                                    </Control.Template>\r\n" \
    "                                </Control>\r\n"

static const char DEV_BADGE_LIST_OLD[] =
    "                                <local:FZTextBlock x:Name=\"PlayerName\" Text=\"{Binding UserProfile.FormattedGamertag}\" Style=\"{StaticResource A_Style_FZTextBlock_PlayerList}\" />\r\n";
static const char DEV_BADGE_LIST_NEW[] =
    "                                <Control VerticalAlignment=\"Center\">\r\n"
    "                                    <Control.Template>\r\n"
    "                                        <ControlTemplate TargetType=\"{x:Type Control}\">\r\n"
    "                                            <StackPanel Orientation=\"Horizontal\">\r\n"
    "                                                <local:FZTextBlock x:Name=\"FH2MP_DevName\" Text=\"{Binding UserProfile.FormattedGamertag}\" Style=\"{StaticResource A_Style_FZTextBlock_PlayerList}\" VerticalAlignment=\"Center\" />\r\n"
    "                                                <Viewbox x:Name=\"FH2MP_DevBadge\" Width=\"17\" Height=\"17\" Margin=\"6,0,0,0\" VerticalAlignment=\"Center\" Visibility=\"Collapsed\">\r\n"
    "                                                    <Grid Width=\"180\" Height=\"180\">\r\n"
    DEV_BADGE_ROW_LOGO DEV_BADGE_ROW_HEAD_B DEV_BADGE_NAMES(DEV_BADGE_ROW_TRIGGER) DEV_BADGE_ROW_TAIL;

static const char DEV_BADGE_BOARD_OLD[] =
    "                    <local:FZTextBlock Text=\"{Binding UserProfile.FormattedGamertag}\" Style=\"{StaticResource A_Style_FZTextBlock_Primary}\" VerticalAlignment=\"Center\" />\r\n";
static const char DEV_BADGE_BOARD_NEW[] =
    "                                <Control VerticalAlignment=\"Center\">\r\n"
    "                                    <Control.Template>\r\n"
    "                                        <ControlTemplate TargetType=\"{x:Type Control}\">\r\n"
    "                                            <StackPanel Orientation=\"Horizontal\">\r\n"
    "                                                <local:FZTextBlock x:Name=\"FH2MP_DevName\" Text=\"{Binding UserProfile.FormattedGamertag}\" Style=\"{StaticResource A_Style_FZTextBlock_Primary}\" VerticalAlignment=\"Center\" />\r\n"
    "                                                <Viewbox x:Name=\"FH2MP_DevBadge\" Width=\"17\" Height=\"17\" Margin=\"6,0,0,0\" VerticalAlignment=\"Center\" Visibility=\"Collapsed\">\r\n"
    "                                                    <Grid Width=\"180\" Height=\"180\">\r\n"
    DEV_BADGE_ROW_LOGO DEV_BADGE_ROW_HEAD_B DEV_BADGE_NAMES(DEV_BADGE_ROW_TRIGGER) DEV_BADGE_ROW_TAIL;
