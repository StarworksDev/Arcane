// Arcane engine CI - multibranch pipeline over github.com/T3mps/Arcane
// (extraction spec: docs/specs/2026-08-11-arcane-repo-extraction-design.md).
// windows-1 carries a GPU, so ArcaneTests runs INCLUDING [gpu] tags and the
// scripted --frames GPU-verify runs against ReferenceProject.
// Stage order is cheapest-failure-first.

pipeline {
    agent none

    options {
        buildDiscarder(logRotator(numToKeepStr: '30'))
        disableConcurrentBuilds()
        timestamps()
        skipDefaultCheckout(true)
    }

    stages {
        stage('Windows') {
            agent { label 'windows && gpu' }
            environment {
                _APH_NOPAUSE = '1'
            }
            stages {
                stage('Checkout') {
                    steps { checkout scm }
                }
                stage('Provision (vcpkg SDL3)') {
                    // Self-heal: build SDL3 with the repo's static-md overlay
                    // triplet (SDL3 itself as a DLL) if it is absent from the
                    // agent's vcpkg -- or still the old static flavor, which the
                    // script replaces. Guarded on presence -- the from-source
                    // build is expensive.
                    steps {
                        bat 'if not exist "%VCPKG_ROOT%\\installed\\x64-windows-static-md\\bin\\SDL3.dll" ( call scripts\\setup-vcpkg-deps.bat ) else ( echo [provision] SDL3 present -- skipping vcpkg build )'
                    }
                }
                stage('Generate') {
                    steps {
                        bat 'call GenerateProjects.bat'
                    }
                }
                stage('Build') {
                    steps {
                        bat 'ci\\msbuild.cmd Arcane.slnx /p:Configuration=Debug   /m /v:minimal /nologo'
                        bat 'ci\\msbuild.cmd Arcane.slnx /p:Configuration=Release /m /v:minimal /nologo'
                        bat 'ci\\msbuild.cmd Arcane.slnx /p:Configuration=Dist    /m /v:minimal /nologo'
                    }
                }
                stage('Asset cook check') {
                    // F2b Task 14 (design doc :318): arccook --check is the nothing-stale
                    // gate. ArcaneRuntime/ArcaneEditor/ArcaneTests each carry a postbuild
                    // hook that just cooked ReferenceProject's Content/ into
                    // Intermediate/Artifacts as a side effect of the Build stage above --
                    // this re-asks CookSession::CheckProject the same question
                    // CookProject just answered, so a broken or incomplete postbuild cook
                    // fails HERE, not three stages downstream as a wrong-looking golden
                    // gate render. Exit 0 clean, 2 stale, 1 broken (bad --project etc.) --
                    // `bat` already fails the build on any nonzero exit, so no explicit
                    // errorlevel handling is needed.
                    //
                    // Debug + Release only, mirroring the Tests stage below: Dist builds
                    // in this pipeline but is never gated further once it does (same gap
                    // the ~[gpu] baseline note already documents).
                    steps {
                        bat 'Binaries\\Debug-windows-x86_64-md\\arccook\\arccook.exe   --project ReferenceProject --check'
                        bat 'Binaries\\Release-windows-x86_64-md\\arccook\\arccook.exe --project ReferenceProject --check'
                    }
                }
                stage('Tests (incl [gpu])') {
                    steps {
                        bat 'if not exist test-results mkdir test-results'
                        // Suites run FROM the exe dir (fixtures + shaders are
                        // staged relative to it). GPU tests assert
                        // Arcane::RenderErrorCount() == 0 -- validation noise
                        // is a test failure by design.
                        // Two -r flags in one invocation: JUnit for Jenkins own test view,
                        // JSON for the baseline check below. Confirmed working on the
                        // vendored Catch2 3.15.0 by RUNNING it, not by reading docs.
                        bat 'cd Binaries\\Debug-windows-x86_64-md\\ArcaneTests   && ArcaneTests.exe -r junit::out=%WORKSPACE%\\test-results\\arcane-debug.xml -r json::out=%WORKSPACE%\\test-results\\arcane-debug.json'
                        bat 'cd Binaries\\Release-windows-x86_64-md\\ArcaneTests && ArcaneTests.exe -r junit::out=%WORKSPACE%\\test-results\\arcane-release.xml -r json::out=%WORKSPACE%\\test-results\\arcane-release.json'
                        // -Invocation "unfiltered" NAMES WHAT THE TWO LINES
                        // ABOVE ACTUALLY RUN: a bare ArcaneTests.exe, the suite
                        // UNFILTERED, which adds ~62000 [gpu] assertions on this
                        // agent. The unfiltered Debug and Release rows live in
                        // scripts/automation-baselines.json. A lane with NO
                        // baseline row FAILS this stage (exit 1) unless
                        // scripts/baseline-exemptions.json names it with a reason;
                        // a missing entry no longer exits 0.
                        bat 'powershell -ExecutionPolicy Bypass -File scripts\\check-baselines.ps1 -ReportPath "%WORKSPACE%\\test-results\\arcane-debug.json" -Configuration Debug -Invocation "unfiltered"'
                        bat 'powershell -ExecutionPolicy Bypass -File scripts\\check-baselines.ps1 -ReportPath "%WORKSPACE%\\test-results\\arcane-release.json" -Configuration Release -Invocation "unfiltered"'
                    }
                    post {
                        // Same rationale as the Golden gate stage's post block below:
                        // the [gpu][thumbs][golden] case in THIS stage writes its own
                        // -actual/-diff PNGs under
                        // Binaries/<Config>/ArcaneTests/ReferenceProject/Saved/Verify/thumbs/,
                        // and a red here fails the BUILD before the Golden gate stage
                        // (and its post block) ever runs -- so without this block the
                        // pictures are written and then wiped with the workspace, and a
                        // CI failure of the thumbnail set arrives as a diffCount in the
                        // JUnit XML with no image to look at.
                        always {
                            archiveArtifacts artifacts: 'Binaries/**/ReferenceProject/Saved/Verify/**/*.png',
                                              allowEmptyArchive: true
                        }
                    }
                }
                stage('Golden gate') {
                    // Task 12 (plan-b comparator): the HOST-LEVEL half of the
                    // golden-image gate. The [gpu][golden] Catch2 case in the
                    // stage above proves the render path compiles and matches
                    // its own committed reference, but ArcaneTests links
                    // neither RuntimeApp nor EditorApp -- it is silent about
                    // boot, settle, --report/--compare and the CLI. THIS
                    // stage is what actually launches both real hosts, on
                    // both backends, the way an agent would, and fails the
                    // build on any HARD-GATING comparison regressing.
                    //
                    // scripts/golden-gate.ps1 enforces its own precondition
                    // (a fresh ReferenceProject/Binaries/ rebuild for the
                    // configuration it is about to run, since that folder is
                    // a single slot shared across configs) before launching
                    // anything, so no separate step is needed here for that.
                    //
                    // ORDER IS LOAD-BEARING: RELEASE THEN DEBUG, never the
                    // reverse. Whichever config runs LAST is the one that
                    // wins the single Binaries/ slot, and the very next
                    // stage -- "ReferenceProject (SDK build)", pre-existing
                    // and unmodified -- does a PLAIN (non-Rebuild) `msbuild
                    // /p:Configuration=Debug` on it, then "Scripted
                    // GPU-verify" launches the Debug ArcaneRuntime.exe
                    // against whatever that produced. A plain build on an
                    // already-current Debug tree can silently no-op ("all
                    // outputs up-to-date") rather than actually rebuild, so
                    // if this stage left the slot in RELEASE state, that
                    // next stage would hand the Debug host a stale Release
                    // ReferenceGame.dll -- the exact single-slot staleness
                    // class this script's own /t:Rebuild exists to prevent,
                    // reintroduced one stage downstream. Running Release
                    // first and Debug last leaves the slot in the state the
                    // rest of the pipeline already assumes.
                    steps {
                        bat 'powershell -NoProfile -ExecutionPolicy Bypass -File scripts\\golden-gate.ps1 -Configuration Release'
                        bat 'powershell -NoProfile -ExecutionPolicy Bypass -File scripts\\golden-gate.ps1 -Configuration Debug'
                    }
                    post {
                        // Diff artifacts land under EACH exe's own directory
                        // (Binaries/<Config>-windows-x86_64-md/<ArcaneRuntime|
                        // ArcaneEditor>/ReferenceProject/Saved/Verify/*.png --
                        // Saved/ is project-gitignored, so this is the only
                        // way a failing lane's diff image survives the
                        // workspace being wiped for the next build).
                        //
                        // `always`, NOT `failure`: archiveArtifacts with
                        // allowEmptyArchive captures whatever diff PNGs
                        // golden-gate.ps1 left on disk, at negligible cost
                        // on a clean run.
                        always {
                            archiveArtifacts artifacts: 'Binaries/**/ReferenceProject/Saved/Verify/**/*.png',
                                              allowEmptyArchive: true
                        }
                    }
                }
                stage('Golden gate self-test') {
                    // A gate never observed failing is not a gate. This breaks
                    // the scene on purpose and asserts all four lanes catch it.
                    //
                    // main/milestone ONLY: the property under test belongs to
                    // the GATE, which changes rarely, and the Golden gate
                    // stage above already runs the four-lane gate TWICE
                    // (Release then Debug) -- this is a third run, so the
                    // added cost is roughly +50%, not a doubling.
                    when { anyOf { branch 'main'; branch 'milestone/*' } }
                    steps {
                        bat 'powershell -NoProfile -ExecutionPolicy Bypass -File scripts\\golden-gate.ps1 -Configuration Debug -SelfTest'
                    }
                }
                stage('ReferenceProject (SDK build)') {
                    // The in-repo external-project consumer: its own premake
                    // workspace over build/arcane.lua -> Binaries/ReferenceGame.dll.
                    steps {
                        bat 'cd ReferenceProject && ..\\ThirdParty\\premake5\\premake5.exe vs2026'
                        bat 'ci\\msbuild.cmd ReferenceProject\\ReferenceProject.slnx /p:Configuration=Debug /m /v:minimal /nologo'
                    }
                }
                stage('Scripted GPU-verify (--frames)') {
                    steps {
                        bat 'Binaries\\Debug-windows-x86_64-md\\ArcaneRuntime\\ArcaneRuntime.exe --project ReferenceProject --frames 180'
                    }
                }
            }
            post {
                always {
                    junit allowEmptyResults: true, testResults: 'test-results/*.xml'
                }
            }
        }

        stage('Linux lane check') {
            // Same plumbing proof as Aphelyon's pipeline: skip when linux-1 is
            // offline so the engine job never queues forever. Real Linux
            // build/test is the Linux-port milestone (direction record step 7).
            when { beforeAgent true; expression { !nodesByLabel(label: 'linux', offline: false).isEmpty() } }
            agent { label 'linux' }
            steps {
                sh 'g++ --version | head -1 && clang++ --version | head -1 && premake5 --version && echo LINUX LANE READY'
            }
        }
    }

    post {
        // OPTIONAL: the same discord-webhook credential Aphelyon uses. A
        // missing plugin or secret must never turn a green engine build red.
        failure {
            script {
                try {
                    withCredentials([string(credentialsId: 'discord-webhook', variable: 'DISCORD_URL')]) {
                        def sha = (env.GIT_COMMIT ?: 'unknown').take(8)
                        discordSend webhookURL: env.DISCORD_URL,
                                    title: "FAILED: ${env.JOB_NAME} #${env.BUILD_NUMBER}",
                                    description: "Branch `${env.BRANCH_NAME}` is red at `${sha}`.\nFollow the link -> Pipeline Overview shows the failing stage; commits below.",
                                    showChangeset: true,
                                    link: env.BUILD_URL, result: 'FAILURE'
                    }
                } catch (Exception e) {
                    echo "Discord notification skipped (not configured): ${e.message}"
                }
            }
        }
        fixed {
            script {
                try {
                    withCredentials([string(credentialsId: 'discord-webhook', variable: 'DISCORD_URL')]) {
                        def sha = (env.GIT_COMMIT ?: 'unknown').take(8)
                        discordSend webhookURL: env.DISCORD_URL,
                                    title: "RECOVERED: ${env.JOB_NAME} #${env.BUILD_NUMBER}",
                                    description: "Branch `${env.BRANCH_NAME}` is green again at `${sha}`.",
                                    showChangeset: true,
                                    link: env.BUILD_URL, result: 'SUCCESS'
                    }
                } catch (Exception e) {
                    echo "Discord notification skipped (not configured): ${e.message}"
                }
            }
        }
    }
}
