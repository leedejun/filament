/*
* Copyright (C) 2021 The Android Open Source Project
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

// If you are bundling this with rollup, webpack, or esbuild, the following URL should be trimmed.
import { LitElement, html, css } from "https://unpkg.com/lit-element?module";

// To allow the DOM to render before the Filament WASM module is ready, we maintain a little
// queue of tasks that get invoked as soon as the module is done loading.
class FilamentTasks {
    constructor() {
        this.tasks = []
        Filament.init([], () => { for (const task of this.tasks) task(); });
    }
    add(callback) {
        if (!Filament.Engine) {
            this.tasks.push(callback);
        } else {
            callback();
        }
    }
}

// FilamentViewer is a fairly limited web component for showing glTF models with Filament.
//
// To embed a 3D viewer in your web page, simply add something like the following to your HTML,
// similar to an <img> element.
//
//     <filament-viewer src="FlightHelmet.gltf" />
//
// In addition to the src URL, attributes can be used to set up an optional IBL and skybox.
// The documentation for these attributes is in the FilamentViewer constructor.
//
// MISSING FEATURES
// ----------------
// None of the following features are implemented. They would be easy to add.
// - Add auto-fit of 3D model (migrate fitIntoUnitCube into gltfio::FilamentAsset)
// - Use decorator-style properties
// - Expose more animation properties (e.g. enable / disable, selected index)
// - Allow more than one instance of the viewer on a page
// - Support dynamic resize
// - Expose camera properties and clear color
// - Expose more IBL properties
// - Expose directional light properties
// - Allow clients to programatically change properties
// - Allow disabling the tumble controls and add scroll-to-zoom
// - Optional turntable animation
// - Fix the import at the top of the file to support webpack / rollup / esbuild
//
class FilamentViewer extends LitElement {
    constructor() {
        super();
        if (FilamentViewer.count++ > 0) throw Error("Only one instance of FilamentViewer is allowed.");
        this.filamentTasks = new FilamentTasks();
        this.canvasId = "filament-viewer-canvas";
        this.src = null;          // Path to glTF file. (required)
        this.alt = null;          // Alternate canvas content.
        this.ibl = null;          // Path to image based light ktx.
        this.sky = null;          // Path to skybox ktx.
        this.intensity = 30000;   // Intensity of the image based light.
    }

    static get properties() {
        return {
            src: { type: String },
            alt: { type: String },
            ibl: { type: String },
            sky: { type: String },
            intensity: { type: Number },
        }
    }

    firstUpdated() {
        this.filamentTasks.add(this._startFilament.bind(this));
    }

    updated(props) {
        if (props.has("src")) this.filamentTasks.add(this._loadAsset.bind(this));
        if (props.has("ibl")) this.filamentTasks.add(this._loadIbl.bind(this));
        if (props.has("sky")) this.filamentTasks.add(this._loadSky.bind(this));
        if (props.has("intensity") && this.indirectLight) {
            this.indirectLight.setIntensity(this.intensity);
        }
    }

    static get styles() {
        return css`
            :host {
                display: inline-block;
                width: 300px;
                height: 300px;
                border: solid 1px black;
            }
            canvas {
                background: #D9D9D9; /* This is consistent with the default clear color. */
                width: 100%;
                height: 100%;
            }`;
    }

    render() {
        return html`<canvas alt="${this.alt}" id="${this.canvasId}"></canvas>`;
    }

    _startFilament() {
        const LightType = Filament.LightManager$Type;

        const canvas = this.shadowRoot.getElementById(this.canvasId);

        this.engine = Filament.Engine.create(canvas);
        this.scene = this.engine.createScene();
        this.trackball = new Trackball(canvas, { startSpin: 0.035 });
        this.sunlight = Filament.EntityManager.get().create();
        this.scene.addEntity(this.sunlight);
        this.loader = this.engine.createAssetLoader();
        this.cameraEntity = Filament.EntityManager.get().create();
        this.camera = this.engine.createCamera(this.cameraEntity);
        this.swapChain = this.engine.createSwapChain();
        this.renderer = this.engine.createRenderer();
        this.view = this.engine.createView();
        this.view.setVignetteOptions({ midPoint: 0.7, enabled: true });
        this.view.setCamera(this.camera);
        this.view.setScene(this.scene);

        // This color is consistent with the default CSS background color.
        this.renderer.setClearOptions({ clearColor: [0.8, 0.8, 0.8, 1.0], clear: true });

        Filament.LightManager.Builder(LightType.SUN)
            .direction([0, -.7, -.7])
            .sunAngularRadius(1.9)
            .castShadows(true)
            .sunHaloSize(10.0)
            .sunHaloFalloff(80.0)
            .build(this.engine, this.sunlight);

        this._onResized();

        window.requestAnimationFrame(this._renderFrame.bind(this));
    }

    _onResized() {
        const Fov = Filament.Camera$Fov;
        const canvas = this.shadowRoot.getElementById(this.canvasId);
        const dpr = window.devicePixelRatio;
        const width = canvas.width = canvas.clientWidth * dpr;
        const height = canvas.height = canvas.clientHeight * dpr;
        this.view.setViewport([0, 0, width, height]);
        const y = -0.125, eye = [0, y, 1.5], center = [0, y, 0], up = [0, 1, 0];
        this.camera.lookAt(eye, center, up);
        const aspect = width / height;
        const fov = aspect < 1 ? Fov.HORIZONTAL : Fov.VERTICAL;
        this.camera.setProjectionFov(25, aspect, 1.0, 10.0, fov);
    }

    _loadIbl() {
        if (!this.ibl) {
            return;
        }
        if (this.indirectLight) {
            console.info("FilamentViewer does not allow the IBL to be changed.");
            return;
        }
        fetch(this.ibl).then(response => {
            return response.arrayBuffer();
        }).then(arrayBuffer => {
            const ktxData = new Uint8Array(arrayBuffer);
            this.indirectLight = this.engine.createIblFromKtx(ktxData);
            this.indirectLight.setIntensity(this.intensity);
            this.scene.setIndirectLight(this.indirectLight);
        });
    }

    _loadSky() {
        if (!this.sky) {
            return;
        }
        if (this.skybox) {
            console.info("FilamentViewer does not allow the skybox to be changed.");
            return;
        }
        fetch(this.sky).then(response => {
            return response.arrayBuffer();
        }).then(arrayBuffer => {
            const ktxData = new Uint8Array(arrayBuffer);
            this.skybox = this.engine.createSkyFromKtx(ktxData);
            this.scene.setSkybox(this.skybox);
        });
    }

    _loadAsset() {
        if (this.asset) {
            console.info("FilamentViewer does not allow the model to be changed.");
            return;
        }
        fetch(this.src).then(response => {
            return response.arrayBuffer();
        }).then(arrayBuffer => {
            const modelData = new Uint8Array(arrayBuffer);
            if (this.src.endsWith(".glb")) {
                this.asset = this.loader.createAssetFromBinary(modelData);
            } else {
                this.asset = this.loader.createAssetFromJson(modelData);
            }
            this.assetRoot = this.asset.getRoot();

            // Enable shadows on every renderable after everything has been loaded.
            this.asset.loadResources(() => {
                const entities = this.asset.getEntities();
                const rm = this.engine.getRenderableManager();
                for (const entity of entities) {
                    const instance = rm.getInstance(entity);
                    rm.setCastShadows(instance, true);
                    instance.delete();
                }
            });

            this.animator = this.asset.getAnimator();
            this.animationStartTime = Date.now();
        });
    }

    _updateAsset() {
        // Invoke the first glTF animation if it exists.
        if (this.animator.getAnimationCount() > 0) {
            const ms = Date.now() - this.animationStartTime;
            this.animator.applyAnimation(0, ms / 1000);
            this.animator.updateBoneMatrices();
        }

        // Tumble the model according to the trackball controller.
        if (this.trackball) {
            const tcm = this.engine.getTransformManager();
            const inst = tcm.getInstance(this.assetRoot);
            tcm.setTransform(inst, this.trackball.getMatrix());
            inst.delete();
        }

        // Add renderable entities to the scene as they become ready.
        while (true) {
            const entity = this.asset.popRenderable();
            if (entity.getId() == 0) {
                entity.delete();
                break;
            }
            this.scene.addEntity(entity);
            entity.delete();
        }
    }

    _renderFrame() {
        // Apply transforms and add entities to the scene.
        if (this.asset) {
            this._updateAsset();
        }

        // Render the view and update the Filament engine.
        if (this.renderer.beginFrame(this.swapChain)) {
            this.renderer.renderView(this.view);
            this.renderer.endFrame();
        }
        this.engine.execute();

        window.requestAnimationFrame(this._renderFrame.bind(this));
    }
}

FilamentViewer.count = 0;

customElements.define("filament-viewer", FilamentViewer);
