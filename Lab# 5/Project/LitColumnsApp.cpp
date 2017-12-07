//***************************************************************************************
// LitColumnsApp.cpp by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

#include "../../Common/d3dApp.h"
#include "../../Common/MathHelper.h"
#include "../../Common/UploadBuffer.h"
#include "../../Common/GeometryGenerator.h"
#include "FrameResource.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "D3D12.lib")

const int gNumFrameResources = 3;

// Lightweight structure stores parameters to draw a shape.  This will
// vary from app-to-app.
struct RenderItem
{
	RenderItem() = default;

    // World matrix of the shape that describes the object's local space
    // relative to the world space, which defines the position, orientation,
    // and scale of the object in the world.
    XMFLOAT4X4 World = MathHelper::Identity4x4();

	XMFLOAT4X4 TexTransform = MathHelper::Identity4x4();

	// Dirty flag indicating the object data has changed and we need to update the constant buffer.
	// Because we have an object cbuffer for each FrameResource, we have to apply the
	// update to each FrameResource.  Thus, when we modify obect data we should set 
	// NumFramesDirty = gNumFrameResources so that each frame resource gets the update.
	int NumFramesDirty = gNumFrameResources;

	// Index into GPU constant buffer corresponding to the ObjectCB for this render item.
	UINT ObjCBIndex = -1;

	Material* Mat = nullptr;
	MeshGeometry* Geo = nullptr;

    // Primitive topology.
    D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

    // DrawIndexedInstanced parameters.
    UINT IndexCount = 0;
    UINT StartIndexLocation = 0;
    int BaseVertexLocation = 0;
};

class LitColumnsApp : public D3DApp
{
public:
    LitColumnsApp(HINSTANCE hInstance);
    LitColumnsApp(const LitColumnsApp& rhs) = delete;
    LitColumnsApp& operator=(const LitColumnsApp& rhs) = delete;
    ~LitColumnsApp();

    virtual bool Initialize()override;

private:
    virtual void OnResize()override;
    virtual void Update(const GameTimer& gt)override;
    virtual void Draw(const GameTimer& gt)override;

    virtual void OnMouseDown(WPARAM btnState, int x, int y)override;
    virtual void OnMouseUp(WPARAM btnState, int x, int y)override;
    virtual void OnMouseMove(WPARAM btnState, int x, int y)override;

    void OnKeyboardInput(const GameTimer& gt);
	void UpdateCamera(const GameTimer& gt);
	void AnimateMaterials(const GameTimer& gt);
	void UpdateObjectCBs(const GameTimer& gt);
	void UpdateMaterialCBs(const GameTimer& gt);
	void UpdateMainPassCB(const GameTimer& gt);

    void BuildRootSignature();
    void BuildShadersAndInputLayout();
    void BuildShapeGeometry();
	void BuildSkullGeometry();
    void BuildPSOs();
    void BuildFrameResources();
    void BuildMaterials();
    void BuildRenderItems();
    void DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems);
 
private:

    std::vector<std::unique_ptr<FrameResource>> mFrameResources;
    FrameResource* mCurrFrameResource = nullptr;
    int mCurrFrameResourceIndex = 0;

    UINT mCbvSrvDescriptorSize = 0;

    ComPtr<ID3D12RootSignature> mRootSignature = nullptr;

	ComPtr<ID3D12DescriptorHeap> mSrvDescriptorHeap = nullptr;

	std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;
	std::unordered_map<std::string, std::unique_ptr<Material>> mMaterials;
	std::unordered_map<std::string, std::unique_ptr<Texture>> mTextures;
	std::unordered_map<std::string, ComPtr<ID3DBlob>> mShaders;

    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

    ComPtr<ID3D12PipelineState> mOpaquePSO = nullptr;
 
	// List of all the render items.
	std::vector<std::unique_ptr<RenderItem>> mAllRitems;

	// Render items divided by PSO.
	std::vector<RenderItem*> mOpaqueRitems;

    PassConstants mMainPassCB;

	XMFLOAT3 mEyePos = { 0.0f, 0.0f, 0.0f };
	XMFLOAT4X4 mView = MathHelper::Identity4x4();
	XMFLOAT4X4 mProj = MathHelper::Identity4x4();

    float mTheta = 1.5f*XM_PI;
    float mPhi = 0.2f*XM_PI;
    float mRadius = 15.0f;

    POINT mLastMousePos;
};

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance,
    PSTR cmdLine, int showCmd)
{
    // Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    try
    {
        LitColumnsApp theApp(hInstance);
        if(!theApp.Initialize())
            return 0;

        return theApp.Run();
    }
    catch(DxException& e)
    {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
        return 0;
    }
}

LitColumnsApp::LitColumnsApp(HINSTANCE hInstance)
    : D3DApp(hInstance)
{
}

LitColumnsApp::~LitColumnsApp()
{
    if(md3dDevice != nullptr)
        FlushCommandQueue();
}

bool LitColumnsApp::Initialize()
{
    if(!D3DApp::Initialize())
        return false;

    // Reset the command list to prep for initialization commands.
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

    // Get the increment size of a descriptor in this heap type.  This is hardware specific, 
	// so we have to query this information.
    mCbvSrvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    BuildRootSignature();
    BuildShadersAndInputLayout();
    BuildShapeGeometry();
	BuildSkullGeometry();
	BuildMaterials();
    BuildRenderItems();
    BuildFrameResources();
    BuildPSOs();

    // Execute the initialization commands.
    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    // Wait until initialization is complete.
    FlushCommandQueue();

    return true;
}
 
void LitColumnsApp::OnResize()
{
    D3DApp::OnResize();

    // The window resized, so update the aspect ratio and recompute the projection matrix.
    XMMATRIX P = XMMatrixPerspectiveFovLH(0.25f*MathHelper::Pi, AspectRatio(), 1.0f, 1000.0f);
    XMStoreFloat4x4(&mProj, P);
}

void LitColumnsApp::Update(const GameTimer& gt)
{
    OnKeyboardInput(gt);
	UpdateCamera(gt);

    // Cycle through the circular frame resource array.
    mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
    mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();

    // Has the GPU finished processing the commands of the current frame resource?
    // If not, wait until the GPU has completed commands up to this fence point.
    if(mCurrFrameResource->Fence != 0 && mFence->GetCompletedValue() < mCurrFrameResource->Fence)
    {
        HANDLE eventHandle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);
        ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->Fence, eventHandle));
        WaitForSingleObject(eventHandle, INFINITE);
        CloseHandle(eventHandle);
    }

	AnimateMaterials(gt);
	UpdateObjectCBs(gt);
	UpdateMaterialCBs(gt);
	UpdateMainPassCB(gt);
}

void LitColumnsApp::Draw(const GameTimer& gt)
{
    auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;

    // Reuse the memory associated with command recording.
    // We can only reset when the associated command lists have finished execution on the GPU.
    ThrowIfFailed(cmdListAlloc->Reset());

    // A command list can be reset after it has been added to the command queue via ExecuteCommandList.
    // Reusing the command list reuses memory.
    ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), mOpaquePSO.Get()));

    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);

    // Indicate a state transition on the resource usage.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

    // Clear the back buffer and depth buffer.
    mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::LightSteelBlue, 0, nullptr);
    mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    // Specify the buffers we are going to render to.
    mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());

	mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

	auto passCB = mCurrFrameResource->PassCB->Resource();
	mCommandList->SetGraphicsRootConstantBufferView(2, passCB->GetGPUVirtualAddress());

    DrawRenderItems(mCommandList.Get(), mOpaqueRitems);

    // Indicate a state transition on the resource usage.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

    // Done recording commands.
    ThrowIfFailed(mCommandList->Close());

    // Add the command list to the queue for execution.
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    // Swap the back and front buffers
    ThrowIfFailed(mSwapChain->Present(0, 0));
	mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

    // Advance the fence value to mark commands up to this fence point.
    mCurrFrameResource->Fence = ++mCurrentFence;

    // Add an instruction to the command queue to set a new fence point. 
    // Because we are on the GPU timeline, the new fence point won't be 
    // set until the GPU finishes processing all the commands prior to this Signal().
    mCommandQueue->Signal(mFence.Get(), mCurrentFence);
}

void LitColumnsApp::OnMouseDown(WPARAM btnState, int x, int y)
{
    mLastMousePos.x = x;
    mLastMousePos.y = y;

    SetCapture(mhMainWnd);
}

void LitColumnsApp::OnMouseUp(WPARAM btnState, int x, int y)
{
    ReleaseCapture();
}

void LitColumnsApp::OnMouseMove(WPARAM btnState, int x, int y)
{
    if((btnState & MK_LBUTTON) != 0)
    {
        // Make each pixel correspond to a quarter of a degree.
        float dx = XMConvertToRadians(0.25f*static_cast<float>(x - mLastMousePos.x));
        float dy = XMConvertToRadians(0.25f*static_cast<float>(y - mLastMousePos.y));

        // Update angles based on input to orbit camera around box.
        mTheta += dx;
        mPhi += dy;

        // Restrict the angle mPhi.
        mPhi = MathHelper::Clamp(mPhi, 0.1f, MathHelper::Pi - 0.1f);
    }
    else if((btnState & MK_RBUTTON) != 0)
    {
        // Make each pixel correspond to 0.2 unit in the scene.
        float dx = 0.05f*static_cast<float>(x - mLastMousePos.x);
        float dy = 0.05f*static_cast<float>(y - mLastMousePos.y);

        // Update the camera radius based on input.
        mRadius += dx - dy;

        // Restrict the radius.
        mRadius = MathHelper::Clamp(mRadius, 5.0f, 150.0f);
    }

    mLastMousePos.x = x;
    mLastMousePos.y = y;
}
 
void LitColumnsApp::OnKeyboardInput(const GameTimer& gt)
{
}
 
void LitColumnsApp::UpdateCamera(const GameTimer& gt)
{
	// Convert Spherical to Cartesian coordinates.
	mEyePos.x = mRadius*sinf(mPhi)*cosf(mTheta);
	mEyePos.z = mRadius*sinf(mPhi)*sinf(mTheta);
	mEyePos.y = mRadius*cosf(mPhi);

	// Build the view matrix.
	XMVECTOR pos = XMVectorSet(mEyePos.x, mEyePos.y, mEyePos.z, 1.0f);
	XMVECTOR target = XMVectorZero();
	XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

	XMMATRIX view = XMMatrixLookAtLH(pos, target, up);
	XMStoreFloat4x4(&mView, view);
}

void LitColumnsApp::AnimateMaterials(const GameTimer& gt)
{
	
}

void LitColumnsApp::UpdateObjectCBs(const GameTimer& gt)
{
	auto currObjectCB = mCurrFrameResource->ObjectCB.get();
	for(auto& e : mAllRitems)
	{
		// Only update the cbuffer data if the constants have changed.  
		// This needs to be tracked per frame resource.
		if(e->NumFramesDirty > 0)
		{
			XMMATRIX world = XMLoadFloat4x4(&e->World);
			XMMATRIX texTransform = XMLoadFloat4x4(&e->TexTransform);

			ObjectConstants objConstants;
			XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(world));
			XMStoreFloat4x4(&objConstants.TexTransform, XMMatrixTranspose(texTransform));

			currObjectCB->CopyData(e->ObjCBIndex, objConstants);

			// Next FrameResource need to be updated too.
			e->NumFramesDirty--;
		}
	}
}

void LitColumnsApp::UpdateMaterialCBs(const GameTimer& gt)
{
	auto currMaterialCB = mCurrFrameResource->MaterialCB.get();
	for(auto& e : mMaterials)
	{
		// Only update the cbuffer data if the constants have changed.  If the cbuffer
		// data changes, it needs to be updated for each FrameResource.
		Material* mat = e.second.get();
		if(mat->NumFramesDirty > 0)
		{
			XMMATRIX matTransform = XMLoadFloat4x4(&mat->MatTransform);

			MaterialConstants matConstants;
			matConstants.DiffuseAlbedo = mat->DiffuseAlbedo;
			matConstants.FresnelR0 = mat->FresnelR0;
			matConstants.Roughness = mat->Roughness;
			XMStoreFloat4x4(&matConstants.MatTransform, XMMatrixTranspose(matTransform));

			currMaterialCB->CopyData(mat->MatCBIndex, matConstants);

			// Next FrameResource need to be updated too.
			mat->NumFramesDirty--;
		}
	}
}

void LitColumnsApp::UpdateMainPassCB(const GameTimer& gt)
{
	XMMATRIX view = XMLoadFloat4x4(&mView);
	XMMATRIX proj = XMLoadFloat4x4(&mProj);

	XMMATRIX viewProj = XMMatrixMultiply(view, proj);
	XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
	XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
	XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

	XMStoreFloat4x4(&mMainPassCB.View, XMMatrixTranspose(view));
	XMStoreFloat4x4(&mMainPassCB.InvView, XMMatrixTranspose(invView));
	XMStoreFloat4x4(&mMainPassCB.Proj, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&mMainPassCB.InvProj, XMMatrixTranspose(invProj));
	XMStoreFloat4x4(&mMainPassCB.ViewProj, XMMatrixTranspose(viewProj));
	XMStoreFloat4x4(&mMainPassCB.InvViewProj, XMMatrixTranspose(invViewProj));
	mMainPassCB.EyePosW = mEyePos;
	mMainPassCB.RenderTargetSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight);
	mMainPassCB.InvRenderTargetSize = XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
	mMainPassCB.NearZ = 1.0f;
	mMainPassCB.FarZ = 1000.0f;
	mMainPassCB.TotalTime = gt.TotalTime();
	mMainPassCB.DeltaTime = gt.DeltaTime();
	mMainPassCB.AmbientLight = { 0.45f, 0.45f, 0.0f, 1.0f };
	mMainPassCB.Lights[0].Direction = { 0.57735f, -0.57735f, 0.57735f };
	mMainPassCB.Lights[0].Strength = { 0.6f, 0.6f, 0.6f };
	mMainPassCB.Lights[1].Direction = { -0.57735f, -0.57735f, 0.57735f };
	mMainPassCB.Lights[1].Strength = { 0.3f, 0.3f, 0.3f };
	mMainPassCB.Lights[2].Direction = { 0.0f, -0.707f, -0.707f };
	mMainPassCB.Lights[2].Strength = { 0.15f, 0.15f, 0.15f };

	auto currPassCB = mCurrFrameResource->PassCB.get();
	currPassCB->CopyData(0, mMainPassCB);
}

void LitColumnsApp::BuildRootSignature()
{
	// Root parameter can be a table, root descriptor or root constants.
	CD3DX12_ROOT_PARAMETER slotRootParameter[3];

	// Create root CBV.
	slotRootParameter[0].InitAsConstantBufferView(0);
	slotRootParameter[1].InitAsConstantBufferView(1);
	slotRootParameter[2].InitAsConstantBufferView(2);

	// A root signature is an array of root parameters.
	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(3, slotRootParameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
	ComPtr<ID3DBlob> serializedRootSig = nullptr;
	ComPtr<ID3DBlob> errorBlob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

	if(errorBlob != nullptr)
	{
		::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
	}
	ThrowIfFailed(hr);

	ThrowIfFailed(md3dDevice->CreateRootSignature(
		0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(mRootSignature.GetAddressOf())));
}

void LitColumnsApp::BuildShadersAndInputLayout()
{
	const D3D_SHADER_MACRO alphaTestDefines[] =
	{
		"ALPHA_TEST", "1",
		NULL, NULL
	};

	mShaders["standardVS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "VS", "vs_5_1");
	mShaders["opaquePS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "PS", "ps_5_1");
	
    mInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
}

void LitColumnsApp::BuildShapeGeometry()
{
    GeometryGenerator geoGen;
	GeometryGenerator::MeshData box = geoGen.CreateBox(1.0f, 1.0f, 1.0f, 3);
	GeometryGenerator::MeshData grid = geoGen.CreateGrid(26.0f, 26.0f, 50, 50);
	GeometryGenerator::MeshData sphere = geoGen.CreateSphere(0.5f, 20, 20);
	
	GeometryGenerator::MeshData diamond = geoGen.CreateDiamondOfDeath(1.25f);
	GeometryGenerator::MeshData cylinder = geoGen.CreateCylinder(1.0f, 1.0f, 1.0f, 20, 20); // Main Pillar, 
	GeometryGenerator::MeshData cone = geoGen.CreateCone(1.0f, 0.5f); // Main Pillar Top, 
	GeometryGenerator::MeshData wedge = geoGen.CreateWedge(1.0f, 1.0f, 1.0f);
	GeometryGenerator::MeshData pyramid = geoGen.CreatePyramid(1.0f, 1.0f, 1.0f);
	GeometryGenerator::MeshData truncPyramid = geoGen.CreateTruncatedPyramid(1.0f, 1.0f, 0.5f, 0.5f, 1.0f);
	GeometryGenerator::MeshData triangularPrism = geoGen.CreateTriangularPrism(1.0f, 1.0f, 1.0f);
	GeometryGenerator::MeshData tetrahedron = geoGen.CreateTetrahedron(1.0f, 1.0f);

	//
	// We are concatenating all the geometry into one big vertex/index buffer.  So
	// define the regions in the buffer each submesh covers.
	//

	// Cache the vertex offsets to each object in the concatenated vertex buffer.
	UINT boxVertexOffset = 0;
	UINT gridVertexOffset = (UINT)box.Vertices.size();
	UINT sphereVertexOffset = gridVertexOffset + (UINT)grid.Vertices.size();
	UINT cylinderVertexOffset = sphereVertexOffset + (UINT)sphere.Vertices.size();
	UINT diamondVertextOffset = cylinderVertexOffset + (UINT)cylinder.Vertices.size();
	UINT coneVertexOffset = diamondVertextOffset + (UINT)diamond.Vertices.size();
	UINT wedgeVertexOffset = coneVertexOffset + (UINT)cone.Vertices.size();
	UINT pyramidVertexOffset = wedgeVertexOffset + (UINT)wedge.Vertices.size();
	UINT truncPyramidVertexOffset = pyramidVertexOffset + (UINT)pyramid.Vertices.size();
	UINT triangularPrismVertexOffset = truncPyramidVertexOffset + (UINT)truncPyramid.Vertices.size();
	UINT tetrahedronVertexOffset = triangularPrismVertexOffset + (UINT)triangularPrism.Vertices.size();

	// Cache the starting index for each object in the concatenated index buffer.
	UINT boxIndexOffset = 0;
	UINT gridIndexOffset = (UINT)box.Indices32.size();
	UINT sphereIndexOffset = gridIndexOffset + (UINT)grid.Indices32.size();
	UINT cylinderIndexOffset = sphereIndexOffset + (UINT)sphere.Indices32.size();
	UINT diamondIndexOffset = cylinderIndexOffset + (UINT)cylinder.Indices32.size();
	UINT coneIndexOffset = diamondIndexOffset + (UINT)diamond.Indices32.size();
	UINT wedgeIndexOffset = coneIndexOffset + (UINT)cone.Indices32.size();
	UINT pyramidIndexOffset = wedgeIndexOffset + (UINT)wedge.Indices32.size();
	UINT truncPyramidIndexOffset = pyramidIndexOffset + (UINT)pyramid.Indices32.size();
	UINT triangularPrismIndexOffset = truncPyramidIndexOffset + (UINT)truncPyramid.Indices32.size();
	UINT tetrahedronIndexOffset = triangularPrismIndexOffset + (UINT)triangularPrism.Indices32.size();

	SubmeshGeometry boxSubmesh;
	boxSubmesh.IndexCount = (UINT)box.Indices32.size();
	boxSubmesh.StartIndexLocation = boxIndexOffset;
	boxSubmesh.BaseVertexLocation = boxVertexOffset;

	SubmeshGeometry gridSubmesh;
	gridSubmesh.IndexCount = (UINT)grid.Indices32.size();
	gridSubmesh.StartIndexLocation = gridIndexOffset;
	gridSubmesh.BaseVertexLocation = gridVertexOffset;

	SubmeshGeometry sphereSubmesh;
	sphereSubmesh.IndexCount = (UINT)sphere.Indices32.size();
	sphereSubmesh.StartIndexLocation = sphereIndexOffset;
	sphereSubmesh.BaseVertexLocation = sphereVertexOffset;

	SubmeshGeometry cylinderSubmesh;
	cylinderSubmesh.IndexCount = (UINT)cylinder.Indices32.size();
	cylinderSubmesh.StartIndexLocation = cylinderIndexOffset;
	cylinderSubmesh.BaseVertexLocation = cylinderVertexOffset;


	SubmeshGeometry diamondSubmesh;
	diamondSubmesh.IndexCount = (UINT)diamond.Indices32.size();
	diamondSubmesh.StartIndexLocation = diamondIndexOffset;
	diamondSubmesh.BaseVertexLocation = diamondVertextOffset;

	SubmeshGeometry coneSubmesh;
	coneSubmesh.IndexCount = (UINT)cone.Indices32.size();
	coneSubmesh.StartIndexLocation = coneIndexOffset;
	coneSubmesh.BaseVertexLocation = coneVertexOffset;

	SubmeshGeometry wedgeSubmesh;
	wedgeSubmesh.IndexCount = (UINT)wedge.Indices32.size();
	wedgeSubmesh.StartIndexLocation = wedgeIndexOffset;
	wedgeSubmesh.BaseVertexLocation = wedgeVertexOffset;

	SubmeshGeometry pyramidSubmesh;
	pyramidSubmesh.IndexCount = (UINT)pyramid.Indices32.size();
	pyramidSubmesh.StartIndexLocation = pyramidIndexOffset;
	pyramidSubmesh.BaseVertexLocation = pyramidVertexOffset;

	SubmeshGeometry truncPyramidSubmesh;
	truncPyramidSubmesh.IndexCount = (UINT)truncPyramid.Indices32.size();
	truncPyramidSubmesh.StartIndexLocation = truncPyramidIndexOffset;
	truncPyramidSubmesh.BaseVertexLocation = truncPyramidVertexOffset;

	SubmeshGeometry triangularPrismSubmesh;
	triangularPrismSubmesh.IndexCount = (UINT)triangularPrism.Indices32.size();
	triangularPrismSubmesh.StartIndexLocation = triangularPrismIndexOffset;
	triangularPrismSubmesh.BaseVertexLocation = triangularPrismVertexOffset;

	SubmeshGeometry tetrahedronSubmesh;
	tetrahedronSubmesh.IndexCount = (UINT)tetrahedron.Indices32.size();
	tetrahedronSubmesh.StartIndexLocation = tetrahedronIndexOffset;
	tetrahedronSubmesh.BaseVertexLocation = tetrahedronVertexOffset;

	//
	// Extract the vertex elements we are interested in and pack the
	// vertices of all the meshes into one vertex buffer.
	//

	auto totalVertexCount =
		box.Vertices.size() +
		grid.Vertices.size() +
		sphere.Vertices.size() +
		cylinder.Vertices.size() +
		diamond.Vertices.size() +
		cone.Vertices.size() +
		wedge.Vertices.size() +
		pyramid.Vertices.size() +
		truncPyramid.Vertices.size() +
		triangularPrism.Vertices.size() +
		tetrahedron.Vertices.size();

	std::vector<Vertex> vertices(totalVertexCount);

	UINT k = 0;
	for(size_t i = 0; i < box.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = box.Vertices[i].Position;
		vertices[k].Normal = box.Vertices[i].Normal;
	}

	for(size_t i = 0; i < grid.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = grid.Vertices[i].Position;
		vertices[k].Normal = grid.Vertices[i].Normal;
	}

	for(size_t i = 0; i < sphere.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = sphere.Vertices[i].Position;
		vertices[k].Normal = sphere.Vertices[i].Normal;
	}

	for(size_t i = 0; i < cylinder.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = cylinder.Vertices[i].Position;
		vertices[k].Normal = cylinder.Vertices[i].Normal;
	}
	for (size_t i = 0; i < diamond.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = diamond.Vertices[i].Position;
		vertices[k].Normal = diamond.Vertices[i].Normal;
	}
	for (size_t i = 0; i < cone.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = cone.Vertices[i].Position;
		vertices[k].Normal = cone.Vertices[i].Normal;
	}
	for (size_t i = 0; i < wedge.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = wedge.Vertices[i].Position;
		vertices[k].Normal = wedge.Vertices[i].Normal;
	}
	for (size_t i = 0; i < pyramid.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = pyramid.Vertices[i].Position;
		vertices[k].Normal = pyramid.Vertices[i].Normal;
	}
	for (size_t i = 0; i < truncPyramid.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = truncPyramid.Vertices[i].Position;
		vertices[k].Normal = truncPyramid.Vertices[i].Normal;
	}
	for (size_t i = 0; i < triangularPrism.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = triangularPrism.Vertices[i].Position;
		vertices[k].Normal = triangularPrism.Vertices[i].Normal;
	}
	for (size_t i = 0; i < tetrahedron.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = tetrahedron.Vertices[i].Position;
		vertices[k].Normal = tetrahedron.Vertices[i].Normal;
	}

	std::vector<std::uint16_t> indices;
	indices.insert(indices.end(), std::begin(box.GetIndices16()), std::end(box.GetIndices16()));
	indices.insert(indices.end(), std::begin(grid.GetIndices16()), std::end(grid.GetIndices16()));
	indices.insert(indices.end(), std::begin(sphere.GetIndices16()), std::end(sphere.GetIndices16()));
	indices.insert(indices.end(), std::begin(cylinder.GetIndices16()), std::end(cylinder.GetIndices16()));
	indices.insert(indices.end(), std::begin(diamond.GetIndices16()), std::end(diamond.GetIndices16()));
	indices.insert(indices.end(), std::begin(cone.GetIndices16()), std::end(cone.GetIndices16()));
	indices.insert(indices.end(), std::begin(wedge.GetIndices16()), std::end(wedge.GetIndices16()));
	indices.insert(indices.end(), std::begin(pyramid.GetIndices16()), std::end(pyramid.GetIndices16()));
	indices.insert(indices.end(), std::begin(truncPyramid.GetIndices16()), std::end(truncPyramid.GetIndices16()));
	indices.insert(indices.end(), std::begin(triangularPrism.GetIndices16()), std::end(triangularPrism.GetIndices16()));
	indices.insert(indices.end(), std::begin(tetrahedron.GetIndices16()), std::end(tetrahedron.GetIndices16()));


    const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
    const UINT ibByteSize = (UINT)indices.size()  * sizeof(std::uint16_t);

	auto geo = std::make_unique<MeshGeometry>();
	geo->Name = "shapeGeo";

	ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
	CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

	ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
	CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

	geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), vertices.data(), vbByteSize, geo->VertexBufferUploader);

	geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), indices.data(), ibByteSize, geo->IndexBufferUploader);

	geo->VertexByteStride = sizeof(Vertex);
	geo->VertexBufferByteSize = vbByteSize;
	geo->IndexFormat = DXGI_FORMAT_R16_UINT;
	geo->IndexBufferByteSize = ibByteSize;

	geo->DrawArgs["box"] = boxSubmesh;
	geo->DrawArgs["grid"] = gridSubmesh;
	geo->DrawArgs["sphere"] = sphereSubmesh;
	geo->DrawArgs["cylinder"] = cylinderSubmesh;
	geo->DrawArgs["diamond"] = diamondSubmesh;
	geo->DrawArgs["cone"] = coneSubmesh;
	geo->DrawArgs["wedge"] = wedgeSubmesh;
	geo->DrawArgs["pyramid"] = pyramidSubmesh;
	geo->DrawArgs["truncPyramid"] = truncPyramidSubmesh;
	geo->DrawArgs["triangularPrism"] = triangularPrismSubmesh;
	geo->DrawArgs["tetrahedron"] = tetrahedronSubmesh;
	

	mGeometries[geo->Name] = std::move(geo);
}

void LitColumnsApp::BuildSkullGeometry()
{
	std::ifstream fin("Models/skull.txt");

	if(!fin)
	{
		MessageBox(0, L"Models/skull.txt not found.", 0, 0);
		return;
	}

	UINT vcount = 0;
	UINT tcount = 0;
	std::string ignore;

	fin >> ignore >> vcount;
	fin >> ignore >> tcount;
	fin >> ignore >> ignore >> ignore >> ignore;

	std::vector<Vertex> vertices(vcount);
	for(UINT i = 0; i < vcount; ++i)
	{
		fin >> vertices[i].Pos.x >> vertices[i].Pos.y >> vertices[i].Pos.z;
		fin >> vertices[i].Normal.x >> vertices[i].Normal.y >> vertices[i].Normal.z;
	}

	fin >> ignore;
	fin >> ignore;
	fin >> ignore;

	std::vector<std::int32_t> indices(3 * tcount);
	for(UINT i = 0; i < tcount; ++i)
	{
		fin >> indices[i * 3 + 0] >> indices[i * 3 + 1] >> indices[i * 3 + 2];
	}

	fin.close();

	//
	// Pack the indices of all the meshes into one index buffer.
	//

	const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);

	const UINT ibByteSize = (UINT)indices.size() * sizeof(std::int32_t);

	auto geo = std::make_unique<MeshGeometry>();
	geo->Name = "skullGeo";

	ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
	CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

	ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
	CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

	geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), vertices.data(), vbByteSize, geo->VertexBufferUploader);

	geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), indices.data(), ibByteSize, geo->IndexBufferUploader);

	geo->VertexByteStride = sizeof(Vertex);
	geo->VertexBufferByteSize = vbByteSize;
	geo->IndexFormat = DXGI_FORMAT_R32_UINT;
	geo->IndexBufferByteSize = ibByteSize;

	SubmeshGeometry submesh;
	submesh.IndexCount = (UINT)indices.size();
	submesh.StartIndexLocation = 0;
	submesh.BaseVertexLocation = 0;

	geo->DrawArgs["skull"] = submesh;

	mGeometries[geo->Name] = std::move(geo);
}

void LitColumnsApp::BuildPSOs()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC opaquePsoDesc;

	//
	// PSO for opaque objects.
	//
    ZeroMemory(&opaquePsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
	opaquePsoDesc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
	opaquePsoDesc.pRootSignature = mRootSignature.Get();
	opaquePsoDesc.VS = 
	{ 
		reinterpret_cast<BYTE*>(mShaders["standardVS"]->GetBufferPointer()), 
		mShaders["standardVS"]->GetBufferSize()
	};
	opaquePsoDesc.PS = 
	{ 
		reinterpret_cast<BYTE*>(mShaders["opaquePS"]->GetBufferPointer()),
		mShaders["opaquePS"]->GetBufferSize()
	};
	opaquePsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	opaquePsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	opaquePsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	opaquePsoDesc.SampleMask = UINT_MAX;
	opaquePsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	opaquePsoDesc.NumRenderTargets = 1;
	opaquePsoDesc.RTVFormats[0] = mBackBufferFormat;
	opaquePsoDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
	opaquePsoDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
	opaquePsoDesc.DSVFormat = mDepthStencilFormat;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&opaquePsoDesc, IID_PPV_ARGS(&mOpaquePSO)));
}

void LitColumnsApp::BuildFrameResources()
{
    for(int i = 0; i < gNumFrameResources; ++i)
    {
        mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(),
            1, (UINT)mAllRitems.size(), (UINT)mMaterials.size()));
    }
}

void LitColumnsApp::BuildMaterials()
{
	auto bricks0 = std::make_unique<Material>();
	bricks0->Name = "bricks0";
	bricks0->MatCBIndex = 0;
	bricks0->DiffuseSrvHeapIndex = 0;
	bricks0->DiffuseAlbedo = XMFLOAT4(Colors::ForestGreen);
	bricks0->FresnelR0 = XMFLOAT3(0.02f, 0.02f, 0.02f);
	bricks0->Roughness = 0.1f;

	auto stone0 = std::make_unique<Material>();
	stone0->Name = "stone0";
	stone0->MatCBIndex = 1;
	stone0->DiffuseSrvHeapIndex = 1;
	stone0->DiffuseAlbedo = XMFLOAT4(Colors::LightSteelBlue);
	stone0->FresnelR0 = XMFLOAT3(0.05f, 0.05f, 0.05f);
	stone0->Roughness = 0.3f;
 
	auto tile0 = std::make_unique<Material>();
	tile0->Name = "tile0";
	tile0->MatCBIndex = 2;
	tile0->DiffuseSrvHeapIndex = 2;
	tile0->DiffuseAlbedo = XMFLOAT4(Colors::LightGray);
	tile0->FresnelR0 = XMFLOAT3(0.02f, 0.02f, 0.02f);
	tile0->Roughness = 0.2f;

	auto skullMat = std::make_unique<Material>();
	skullMat->Name = "skullMat";
	skullMat->MatCBIndex = 3;
	skullMat->DiffuseSrvHeapIndex = 3;
	skullMat->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
	skullMat->FresnelR0 = XMFLOAT3(0.05f, 0.05f, 0.05f);
	skullMat->Roughness = 0.3f;

	auto diamondMat = std::make_unique<Material>();
	diamondMat->Name = "diamondMat";
	diamondMat->MatCBIndex = 4;
	diamondMat->DiffuseSrvHeapIndex = 4;
	diamondMat->DiffuseAlbedo = XMFLOAT4(0.0f, 0.0f, 1.0f, 1.0f);
	diamondMat->FresnelR0 = XMFLOAT3(0.05f, 0.05f, 0.15f);
	diamondMat->Roughness = 0.9f;

	auto coneMat = std::make_unique<Material>();
	coneMat->Name = "coneMat";
	coneMat->MatCBIndex = 5;
	coneMat->DiffuseSrvHeapIndex = 5;
	coneMat->DiffuseAlbedo = XMFLOAT4(1.0f, 0.0f, 1.0f, 1.0f);
	coneMat->FresnelR0 = XMFLOAT3(0.05f, 0.05f, 0.15f);
	coneMat->Roughness = 0.9f;

	
	mMaterials["bricks0"] = std::move(bricks0);
	mMaterials["stone0"] = std::move(stone0);
	mMaterials["tile0"] = std::move(tile0);
	mMaterials["skullMat"] = std::move(skullMat);
	mMaterials["diamondMat"] = std::move(diamondMat);
	mMaterials["coneMat"] = std::move(coneMat);
	
}

void LitColumnsApp::BuildRenderItems()
{
	// Grid
    auto gridRitem = std::make_unique<RenderItem>();
    gridRitem->World = MathHelper::Identity4x4();
	XMStoreFloat4x4(&gridRitem->TexTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
	gridRitem->ObjCBIndex = 0;
	gridRitem->Mat = mMaterials["tile0"].get();
	gridRitem->Geo = mGeometries["shapeGeo"].get();
	gridRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    gridRitem->IndexCount = gridRitem->Geo->DrawArgs["grid"].IndexCount;
    gridRitem->StartIndexLocation = gridRitem->Geo->DrawArgs["grid"].StartIndexLocation;
    gridRitem->BaseVertexLocation = gridRitem->Geo->DrawArgs["grid"].BaseVertexLocation;
	mAllRitems.push_back(std::move(gridRitem));

	UINT objCBIndex = 1;


	//Main Pillars
	//Back Right cylinder
	auto cylinderBRItem = std::make_unique<RenderItem>();
	XMMATRIX cylinderBRWorld = XMMatrixScaling(1.5f, 6.0f, 1.5f) * XMMatrixTranslation(10.5f, 3.0f, 10.5f);
	XMStoreFloat4x4(&cylinderBRItem->World, cylinderBRWorld);
	cylinderBRItem->TexTransform = MathHelper::Identity4x4();
	cylinderBRItem->ObjCBIndex = objCBIndex++;
	cylinderBRItem->Mat = mMaterials["stone0"].get();
	cylinderBRItem->Geo = mGeometries["shapeGeo"].get();
	cylinderBRItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	cylinderBRItem->IndexCount = cylinderBRItem->Geo->DrawArgs["cylinder"].IndexCount;
	cylinderBRItem->StartIndexLocation = cylinderBRItem->Geo->DrawArgs["cylinder"].StartIndexLocation;
	cylinderBRItem->BaseVertexLocation = cylinderBRItem->Geo->DrawArgs["cylinder"].BaseVertexLocation;
	mAllRitems.push_back(std::move(cylinderBRItem));

	//Back Left cylinder
	auto cylinderBLItem = std::make_unique<RenderItem>();
	XMMATRIX cylinderBLWorld = XMMatrixScaling(1.5f, 6.0f, 1.5f) * XMMatrixTranslation(-10.5f, 3.0f, 10.5f);
	XMStoreFloat4x4(&cylinderBLItem->World, cylinderBLWorld);
	cylinderBLItem->TexTransform = MathHelper::Identity4x4();
	cylinderBLItem->ObjCBIndex = objCBIndex++;
	cylinderBLItem->Mat = mMaterials["stone0"].get();
	cylinderBLItem->Geo = mGeometries["shapeGeo"].get();
	cylinderBLItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	cylinderBLItem->IndexCount = cylinderBLItem->Geo->DrawArgs["cylinder"].IndexCount;
	cylinderBLItem->StartIndexLocation = cylinderBLItem->Geo->DrawArgs["cylinder"].StartIndexLocation;
	cylinderBLItem->BaseVertexLocation = cylinderBLItem->Geo->DrawArgs["cylinder"].BaseVertexLocation;
	mAllRitems.push_back(std::move(cylinderBLItem));

	//Front Right cylinder
	auto cylinderFRItem = std::make_unique<RenderItem>();
	XMMATRIX cylinderFRWorld = XMMatrixScaling(1.5f, 6.0f, 1.5f) * XMMatrixTranslation(10.5f, 3.0f, -10.5f);
	XMStoreFloat4x4(&cylinderFRItem->World, cylinderFRWorld);
	cylinderFRItem->TexTransform = MathHelper::Identity4x4();
	cylinderFRItem->ObjCBIndex = objCBIndex++;
	cylinderFRItem->Mat = mMaterials["stone0"].get();
	cylinderFRItem->Geo = mGeometries["shapeGeo"].get();
	cylinderFRItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	cylinderFRItem->IndexCount = cylinderFRItem->Geo->DrawArgs["cylinder"].IndexCount;
	cylinderFRItem->StartIndexLocation = cylinderFRItem->Geo->DrawArgs["cylinder"].StartIndexLocation;
	cylinderFRItem->BaseVertexLocation = cylinderFRItem->Geo->DrawArgs["cylinder"].BaseVertexLocation;
	mAllRitems.push_back(std::move(cylinderFRItem));

	//Front Left cylinder
	auto cylinderFLItem = std::make_unique<RenderItem>();
	XMMATRIX cylinderFLWorld = XMMatrixScaling(1.5f, 6.0f, 1.5f) * XMMatrixTranslation(-10.5f, 3.0f, -10.5f);
	XMStoreFloat4x4(&cylinderFLItem->World, cylinderFLWorld);
	cylinderFLItem->TexTransform = MathHelper::Identity4x4();
	cylinderFLItem->ObjCBIndex = objCBIndex++;
	cylinderFLItem->Mat = mMaterials["stone0"].get();
	cylinderFLItem->Geo = mGeometries["shapeGeo"].get();
	cylinderFLItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	cylinderFLItem->IndexCount = cylinderFLItem->Geo->DrawArgs["cylinder"].IndexCount;
	cylinderFLItem->StartIndexLocation = cylinderFLItem->Geo->DrawArgs["cylinder"].StartIndexLocation;
	cylinderFLItem->BaseVertexLocation = cylinderFLItem->Geo->DrawArgs["cylinder"].BaseVertexLocation;
	mAllRitems.push_back(std::move(cylinderFLItem));

	//Back Right Cone
	auto coneBRItem = std::make_unique<RenderItem>();
	XMMATRIX coneBRWorld = XMMatrixScaling(3.0f, 2.0f, 3.0f) * XMMatrixTranslation(10.5f, 7.0f, 10.5f);
	XMStoreFloat4x4(&coneBRItem->World, coneBRWorld);
	coneBRItem->TexTransform = MathHelper::Identity4x4();
	coneBRItem->ObjCBIndex = objCBIndex++;
	coneBRItem->Mat = mMaterials["coneMat"].get();
	coneBRItem->Geo = mGeometries["shapeGeo"].get();
	coneBRItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	coneBRItem->IndexCount = coneBRItem->Geo->DrawArgs["cone"].IndexCount;
	coneBRItem->StartIndexLocation = coneBRItem->Geo->DrawArgs["cone"].StartIndexLocation;
	coneBRItem->BaseVertexLocation = coneBRItem->Geo->DrawArgs["cone"].BaseVertexLocation;
	mAllRitems.push_back(std::move(coneBRItem));
	
	//Back Left Cone
	auto coneBLItem = std::make_unique<RenderItem>();
	XMMATRIX coneBLWorld = XMMatrixScaling(3.0f, 2.0f, 3.0f) * XMMatrixTranslation(-10.5f, 7.0f, 10.5f);
	XMStoreFloat4x4(&coneBLItem->World, coneBLWorld);
	coneBLItem->TexTransform = MathHelper::Identity4x4();
	coneBLItem->ObjCBIndex = objCBIndex++;
	coneBLItem->Mat = mMaterials["coneMat"].get();
	coneBLItem->Geo = mGeometries["shapeGeo"].get();
	coneBLItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	coneBLItem->IndexCount = coneBLItem->Geo->DrawArgs["cone"].IndexCount;
	coneBLItem->StartIndexLocation = coneBLItem->Geo->DrawArgs["cone"].StartIndexLocation;
	coneBLItem->BaseVertexLocation = coneBLItem->Geo->DrawArgs["cone"].BaseVertexLocation;
	mAllRitems.push_back(std::move(coneBLItem));
	
	//Front Right Cone
	auto coneFRItem = std::make_unique<RenderItem>();
	XMMATRIX coneFRWorld = XMMatrixScaling(3.0f, 2.0f, 3.0f) * XMMatrixTranslation(10.5f, 7.0f, -10.5f);
	XMStoreFloat4x4(&coneFRItem->World, coneFRWorld);
	coneFRItem->TexTransform = MathHelper::Identity4x4();
	coneFRItem->ObjCBIndex = objCBIndex++;
	coneFRItem->Mat = mMaterials["coneMat"].get();
	coneFRItem->Geo = mGeometries["shapeGeo"].get();
	coneFRItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	coneFRItem->IndexCount = coneFRItem->Geo->DrawArgs["cone"].IndexCount;
	coneFRItem->StartIndexLocation = coneFRItem->Geo->DrawArgs["cone"].StartIndexLocation;
	coneFRItem->BaseVertexLocation = coneFRItem->Geo->DrawArgs["cone"].BaseVertexLocation;
	mAllRitems.push_back(std::move(coneFRItem));

	//Front Left Cone
	auto coneFLItem = std::make_unique<RenderItem>();
	XMMATRIX coneFLWorld = XMMatrixScaling(3.0f, 2.0f, 3.0f) * XMMatrixTranslation(-10.5f, 7.0f, -10.5f);
	XMStoreFloat4x4(&coneFLItem->World, coneFLWorld);
	coneFLItem->TexTransform = MathHelper::Identity4x4();
	coneFLItem->ObjCBIndex = objCBIndex++;
	coneFLItem->Mat = mMaterials["coneMat"].get();
	coneFLItem->Geo = mGeometries["shapeGeo"].get();
	coneFLItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	coneFLItem->IndexCount = coneFLItem->Geo->DrawArgs["cone"].IndexCount;
	coneFLItem->StartIndexLocation = coneFLItem->Geo->DrawArgs["cone"].StartIndexLocation;
	coneFLItem->BaseVertexLocation = coneFLItem->Geo->DrawArgs["cone"].BaseVertexLocation;
	mAllRitems.push_back(std::move(coneFLItem));


	// Walls
	// Left Wall
	auto wallLeftItem = std::make_unique<RenderItem>();
	XMMATRIX wallLeftWorld = XMMatrixScaling(1.5f, 4.0f, 18.5f) * XMMatrixTranslation(-10.5f, 2.0f, 0.0f);
	XMStoreFloat4x4(&wallLeftItem->World, wallLeftWorld);
	wallLeftItem->TexTransform = MathHelper::Identity4x4();
	wallLeftItem->ObjCBIndex = objCBIndex++;
	wallLeftItem->Mat = mMaterials["coneMat"].get();
	wallLeftItem->Geo = mGeometries["shapeGeo"].get();
	wallLeftItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	wallLeftItem->IndexCount = wallLeftItem->Geo->DrawArgs["box"].IndexCount;
	wallLeftItem->StartIndexLocation = wallLeftItem->Geo->DrawArgs["box"].StartIndexLocation;
	wallLeftItem->BaseVertexLocation = wallLeftItem->Geo->DrawArgs["box"].BaseVertexLocation;
	mAllRitems.push_back(std::move(wallLeftItem));

	// Wall Right
	auto wallRightItem = std::make_unique<RenderItem>();
	XMMATRIX wallRightWorld = XMMatrixScaling(1.5f, 4.0f, 18.5f) * XMMatrixTranslation(10.5f, 2.0f, 0.0f);
	XMStoreFloat4x4(&wallRightItem->World, wallRightWorld);
	wallRightItem->TexTransform = MathHelper::Identity4x4();
	wallRightItem->ObjCBIndex = objCBIndex++;
	wallRightItem->Mat = mMaterials["coneMat"].get();
	wallRightItem->Geo = mGeometries["shapeGeo"].get();
	wallRightItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	wallRightItem->IndexCount = wallRightItem->Geo->DrawArgs["box"].IndexCount;
	wallRightItem->StartIndexLocation = wallRightItem->Geo->DrawArgs["box"].StartIndexLocation;
	wallRightItem->BaseVertexLocation = wallRightItem->Geo->DrawArgs["box"].BaseVertexLocation;
	mAllRitems.push_back(std::move(wallRightItem));

	// Wall Back
	auto wallBackItem = std::make_unique<RenderItem>();
	XMMATRIX wallBackWorld = XMMatrixScaling(18.5f, 4.0f, 1.5f) * XMMatrixTranslation(0.0f, 2.0f, 10.5f);
	XMStoreFloat4x4(&wallBackItem->World, wallBackWorld);
	wallBackItem->TexTransform = MathHelper::Identity4x4();
	wallBackItem->ObjCBIndex = objCBIndex++;
	wallBackItem->Mat = mMaterials["coneMat"].get();
	wallBackItem->Geo = mGeometries["shapeGeo"].get();
	wallBackItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	wallBackItem->IndexCount = wallBackItem->Geo->DrawArgs["box"].IndexCount;
	wallBackItem->StartIndexLocation = wallBackItem->Geo->DrawArgs["box"].StartIndexLocation;
	wallBackItem->BaseVertexLocation = wallBackItem->Geo->DrawArgs["box"].BaseVertexLocation;
	mAllRitems.push_back(std::move(wallBackItem));

	// Wall Front Left
	auto wallFLItem = std::make_unique<RenderItem>();
	XMMATRIX wallFLWorld = XMMatrixScaling(7.0f, 3.0f, 1.5f) * XMMatrixTranslation(-5.75f, 2.0f, -10.5f);
	XMStoreFloat4x4(&wallFLItem->World, wallFLWorld);
	wallFLItem->TexTransform = MathHelper::Identity4x4();
	wallFLItem->ObjCBIndex = objCBIndex++;
	wallFLItem->Mat = mMaterials["coneMat"].get();
	wallFLItem->Geo = mGeometries["shapeGeo"].get();
	wallFLItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	wallFLItem->IndexCount = wallFLItem->Geo->DrawArgs["box"].IndexCount;
	wallFLItem->StartIndexLocation = wallFLItem->Geo->DrawArgs["box"].StartIndexLocation;
	wallFLItem->BaseVertexLocation = wallFLItem->Geo->DrawArgs["box"].BaseVertexLocation;
	mAllRitems.push_back(std::move(wallFLItem));

	// Wall Front Right
	auto wallFRItem = std::make_unique<RenderItem>();
	XMMATRIX wallFRWorld = XMMatrixScaling(7.0f, 3.0f, 1.5f) * XMMatrixTranslation(5.75f, 2.0f, -10.5f);
	XMStoreFloat4x4(&wallFRItem->World, wallFRWorld);
	wallFRItem->TexTransform = MathHelper::Identity4x4();
	wallFRItem->ObjCBIndex = objCBIndex++;
	wallFRItem->Mat = mMaterials["coneMat"].get();
	wallFRItem->Geo = mGeometries["shapeGeo"].get();
	wallFRItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	wallFRItem->IndexCount = wallFRItem->Geo->DrawArgs["box"].IndexCount;
	wallFRItem->StartIndexLocation = wallFRItem->Geo->DrawArgs["box"].StartIndexLocation;
	wallFRItem->BaseVertexLocation = wallFRItem->Geo->DrawArgs["box"].BaseVertexLocation;
	mAllRitems.push_back(std::move(wallFRItem));

	// Wall Front Top
	auto wallFTItem = std::make_unique<RenderItem>();
	XMMATRIX wallFTWorld = XMMatrixScaling(18.5f, 0.5f, 1.5f) * XMMatrixTranslation(0.0f, 3.75f, -10.5f);
	XMStoreFloat4x4(&wallFTItem->World, wallFTWorld);
	wallFTItem->TexTransform = MathHelper::Identity4x4();
	wallFTItem->ObjCBIndex = objCBIndex++;
	wallFTItem->Mat = mMaterials["coneMat"].get();
	wallFTItem->Geo = mGeometries["shapeGeo"].get();
	wallFTItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	wallFTItem->IndexCount = wallFTItem->Geo->DrawArgs["box"].IndexCount;
	wallFTItem->StartIndexLocation = wallFTItem->Geo->DrawArgs["box"].StartIndexLocation;
	wallFTItem->BaseVertexLocation = wallFTItem->Geo->DrawArgs["box"].BaseVertexLocation;
	mAllRitems.push_back(std::move(wallFTItem));

	// Wall Front Bottom
	auto wallFBItem = std::make_unique<RenderItem>();
	XMMATRIX wallFBWorld = XMMatrixScaling(18.5f, 0.5f, 1.5f) * XMMatrixTranslation(0.0f, 0.25f, -10.5f);
	XMStoreFloat4x4(&wallFBItem->World, wallFBWorld);
	wallFBItem->TexTransform = MathHelper::Identity4x4();
	wallFBItem->ObjCBIndex = objCBIndex++;
	wallFBItem->Mat = mMaterials["coneMat"].get();
	wallFBItem->Geo = mGeometries["shapeGeo"].get();
	wallFBItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	wallFBItem->IndexCount = wallFBItem->Geo->DrawArgs["box"].IndexCount;
	wallFBItem->StartIndexLocation = wallFBItem->Geo->DrawArgs["box"].StartIndexLocation;
	wallFBItem->BaseVertexLocation = wallFBItem->Geo->DrawArgs["box"].BaseVertexLocation;
	mAllRitems.push_back(std::move(wallFBItem));


	// Wall Tops
	// Left Wall Top

	// For top of walls
	// Triangular prism wall top
	auto triangularPrismBItem = std::make_unique<RenderItem>();
	XMMATRIX triangularPrismBWorld = XMMatrixRotationY(90.0f * (XM_PI / 180.0f)) * XMMatrixScaling(1.5f, 2.0f, 1.5f) *  XMMatrixTranslation(8.2f, 5.0f, 10.5f);
	XMStoreFloat4x4(&triangularPrismBItem->World, triangularPrismBWorld);
	triangularPrismBItem->TexTransform = MathHelper::Identity4x4();
	triangularPrismBItem->ObjCBIndex = objCBIndex++;
	triangularPrismBItem->Mat = mMaterials["coneMat"].get();
	triangularPrismBItem->Geo = mGeometries["shapeGeo"].get();
	triangularPrismBItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	triangularPrismBItem->IndexCount = triangularPrismBItem->Geo->DrawArgs["triangularPrism"].IndexCount;
	triangularPrismBItem->StartIndexLocation = triangularPrismBItem->Geo->DrawArgs["triangularPrism"].StartIndexLocation;
	triangularPrismBItem->BaseVertexLocation = triangularPrismBItem->Geo->DrawArgs["triangularPrism"].BaseVertexLocation;
	mAllRitems.push_back(std::move(triangularPrismBItem));


	// Front
	// Outside Ramp
	auto rampItem = std::make_unique<RenderItem>();
	XMMATRIX rampWorld = XMMatrixScaling(4.75f, 0.5f, 1.5f) * XMMatrixTranslation(0.0f, 0.25, -12.0f);
	XMStoreFloat4x4(&rampItem->World, rampWorld);
	rampItem->TexTransform = MathHelper::Identity4x4();
	rampItem->ObjCBIndex = objCBIndex++;
	rampItem->Mat = mMaterials["coneMat"].get();
	rampItem->Geo = mGeometries["shapeGeo"].get();
	rampItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	rampItem->IndexCount = rampItem->Geo->DrawArgs["wedge"].IndexCount;
	rampItem->StartIndexLocation = rampItem->Geo->DrawArgs["wedge"].StartIndexLocation;
	rampItem->BaseVertexLocation = rampItem->Geo->DrawArgs["wedge"].BaseVertexLocation;
	mAllRitems.push_back(std::move(rampItem));

	auto rampInItem = std::make_unique<RenderItem>();
	XMMATRIX rampInWorld = XMMatrixRotationY(180.0f * (XM_PI / 180.0f)) * XMMatrixScaling(4.75f, 0.5f, 1.5f) * XMMatrixTranslation(0.0f, 0.25, -9.0f);
	XMStoreFloat4x4(&rampInItem->World, rampInWorld);
	rampInItem->TexTransform = MathHelper::Identity4x4();
	rampInItem->ObjCBIndex = objCBIndex++;
	rampInItem->Mat = mMaterials["coneMat"].get();
	rampInItem->Geo = mGeometries["shapeGeo"].get();
	rampInItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	rampInItem->IndexCount = rampInItem->Geo->DrawArgs["wedge"].IndexCount;
	rampInItem->StartIndexLocation = rampInItem->Geo->DrawArgs["wedge"].StartIndexLocation;
	rampInItem->BaseVertexLocation = rampInItem->Geo->DrawArgs["wedge"].BaseVertexLocation;
	mAllRitems.push_back(std::move(rampInItem));
	

	// Castle Wall Back
	auto castleWallBItem = std::make_unique<RenderItem>();
	XMMATRIX castleWallBWorld = XMMatrixScaling(10.0f, 5.0f, 0.5f) * XMMatrixTranslation(0.0f, 2.5f, 7.8f);
	XMStoreFloat4x4(&castleWallBItem->World, castleWallBWorld);
	castleWallBItem->TexTransform = MathHelper::Identity4x4();
	castleWallBItem->ObjCBIndex = objCBIndex++;
	castleWallBItem->Mat = mMaterials["coneMat"].get();
	castleWallBItem->Geo = mGeometries["shapeGeo"].get();
	castleWallBItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	castleWallBItem->IndexCount = castleWallBItem->Geo->DrawArgs["box"].IndexCount;
	castleWallBItem->StartIndexLocation = castleWallBItem->Geo->DrawArgs["box"].StartIndexLocation;
	castleWallBItem->BaseVertexLocation = castleWallBItem->Geo->DrawArgs["box"].BaseVertexLocation;
	mAllRitems.push_back(std::move(castleWallBItem));


	// Castle Wall Right
	auto castleWallRItem = std::make_unique<RenderItem>();
	XMMATRIX castleWallRWorld = XMMatrixScaling(0.5f, 5.0f, 10.0f) * XMMatrixTranslation(5.0f, 2.5f, 3.05f);
	XMStoreFloat4x4(&castleWallRItem->World, castleWallRWorld);
	castleWallRItem->TexTransform = MathHelper::Identity4x4();
	castleWallRItem->ObjCBIndex = objCBIndex++;
	castleWallRItem->Mat = mMaterials["coneMat"].get();
	castleWallRItem->Geo = mGeometries["shapeGeo"].get();
	castleWallRItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	castleWallRItem->IndexCount = castleWallRItem->Geo->DrawArgs["box"].IndexCount;
	castleWallRItem->StartIndexLocation = castleWallRItem->Geo->DrawArgs["box"].StartIndexLocation;
	castleWallRItem->BaseVertexLocation = castleWallRItem->Geo->DrawArgs["box"].BaseVertexLocation;
	mAllRitems.push_back(std::move(castleWallRItem));

	// Castle Wall Left
	auto castleWallLItem = std::make_unique<RenderItem>();
	XMMATRIX castleWallLWorld = XMMatrixScaling(0.5f, 5.0f, 10.0f) * XMMatrixTranslation(-5.0f, 2.5f, 3.05f);
	XMStoreFloat4x4(&castleWallLItem->World, castleWallLWorld);
	castleWallLItem->TexTransform = MathHelper::Identity4x4();
	castleWallLItem->ObjCBIndex = objCBIndex++;
	castleWallLItem->Mat = mMaterials["coneMat"].get();
	castleWallLItem->Geo = mGeometries["shapeGeo"].get();
	castleWallLItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	castleWallLItem->IndexCount = castleWallLItem->Geo->DrawArgs["box"].IndexCount;
	castleWallLItem->StartIndexLocation = castleWallLItem->Geo->DrawArgs["box"].StartIndexLocation;
	castleWallLItem->BaseVertexLocation = castleWallLItem->Geo->DrawArgs["box"].BaseVertexLocation;
	mAllRitems.push_back(std::move(castleWallLItem));


	// Castle Wall Front Left
	auto castleWallFLItem = std::make_unique<RenderItem>();
	XMMATRIX castleWallFLWorld = XMMatrixScaling(4.0f, 5.0f, 0.5f) * XMMatrixTranslation(-3.25f, 2.5f, -2.0f);
	XMStoreFloat4x4(&castleWallFLItem->World, castleWallFLWorld);
	castleWallFLItem->TexTransform = MathHelper::Identity4x4();
	castleWallFLItem->ObjCBIndex = objCBIndex++;
	castleWallFLItem->Mat = mMaterials["coneMat"].get();
	castleWallFLItem->Geo = mGeometries["shapeGeo"].get();
	castleWallFLItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	castleWallFLItem->IndexCount = castleWallFLItem->Geo->DrawArgs["box"].IndexCount;
	castleWallFLItem->StartIndexLocation = castleWallFLItem->Geo->DrawArgs["box"].StartIndexLocation;
	castleWallFLItem->BaseVertexLocation = castleWallFLItem->Geo->DrawArgs["box"].BaseVertexLocation;
	mAllRitems.push_back(std::move(castleWallFLItem));


	// Castle Wall Front Right
	auto castleWallFRItem = std::make_unique<RenderItem>();
	XMMATRIX castleWallFRWorld = XMMatrixScaling(4.0f, 5.0f, 0.5f) * XMMatrixTranslation(3.25f, 2.5f, -2.0f);
	XMStoreFloat4x4(&castleWallFRItem->World, castleWallFRWorld);
	castleWallFRItem->TexTransform = MathHelper::Identity4x4();
	castleWallFRItem->ObjCBIndex = objCBIndex++;
	castleWallFRItem->Mat = mMaterials["coneMat"].get();
	castleWallFRItem->Geo = mGeometries["shapeGeo"].get();
	castleWallFRItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	castleWallFRItem->IndexCount = castleWallFRItem->Geo->DrawArgs["box"].IndexCount;
	castleWallFRItem->StartIndexLocation = castleWallFRItem->Geo->DrawArgs["box"].StartIndexLocation;
	castleWallFRItem->BaseVertexLocation = castleWallFRItem->Geo->DrawArgs["box"].BaseVertexLocation;
	mAllRitems.push_back(std::move(castleWallFRItem));


	//Castle Roof Pyramid
	auto pyramidRoofItem = std::make_unique<RenderItem>();
	XMMATRIX pyramidRoofWorld = XMMatrixScaling(10.5f, 4.0f, 10.5f) * XMMatrixTranslation(0.0f, 7.0f, 2.75f);
	XMStoreFloat4x4(&pyramidRoofItem->World, pyramidRoofWorld);
	pyramidRoofItem->TexTransform = MathHelper::Identity4x4();
	pyramidRoofItem->ObjCBIndex = objCBIndex++;
	pyramidRoofItem->Mat = mMaterials["coneMat"].get();
	pyramidRoofItem->Geo = mGeometries["shapeGeo"].get();
	pyramidRoofItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	pyramidRoofItem->IndexCount = pyramidRoofItem->Geo->DrawArgs["pyramid"].IndexCount;
	pyramidRoofItem->StartIndexLocation = pyramidRoofItem->Geo->DrawArgs["pyramid"].StartIndexLocation;
	pyramidRoofItem->BaseVertexLocation = pyramidRoofItem->Geo->DrawArgs["pyramid"].BaseVertexLocation;
	mAllRitems.push_back(std::move(pyramidRoofItem));

	// Left tower Cube
	auto cubeTowerLItem = std::make_unique<RenderItem>();
	XMMATRIX cubeTowerLWorld = XMMatrixScaling(3.0f, 6.0f, 4.0f) * XMMatrixTranslation(-6.5f, 3.0f, 4.0f);
	XMStoreFloat4x4(&cubeTowerLItem->World, cubeTowerLWorld);
	cubeTowerLItem->TexTransform = MathHelper::Identity4x4();
	cubeTowerLItem->ObjCBIndex = objCBIndex++;
	cubeTowerLItem->Mat = mMaterials["coneMat"].get();
	cubeTowerLItem->Geo = mGeometries["shapeGeo"].get();
	cubeTowerLItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	cubeTowerLItem->IndexCount = cubeTowerLItem->Geo->DrawArgs["box"].IndexCount;
	cubeTowerLItem->StartIndexLocation = cubeTowerLItem->Geo->DrawArgs["box"].StartIndexLocation;
	cubeTowerLItem->BaseVertexLocation = cubeTowerLItem->Geo->DrawArgs["box"].BaseVertexLocation;
	mAllRitems.push_back(std::move(cubeTowerLItem));

	// Left tower Top
	// TruncacatedPyramid 
	auto truncTopLItem = std::make_unique<RenderItem>();
	XMMATRIX truncTopLWorld = XMMatrixScaling(3.0f,3.0f, 4.0f) * XMMatrixTranslation(-6.5f, 7.5f, 4.0f);
	XMStoreFloat4x4(&truncTopLItem->World, truncTopLWorld);
	truncTopLItem->TexTransform = MathHelper::Identity4x4();
	truncTopLItem->ObjCBIndex = objCBIndex++;
	truncTopLItem->Mat = mMaterials["coneMat"].get();
	truncTopLItem->Geo = mGeometries["shapeGeo"].get();
	truncTopLItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	truncTopLItem->IndexCount = truncTopLItem->Geo->DrawArgs["truncPyramid"].IndexCount;
	truncTopLItem->StartIndexLocation = truncTopLItem->Geo->DrawArgs["truncPyramid"].StartIndexLocation;
	truncTopLItem->BaseVertexLocation = truncTopLItem->Geo->DrawArgs["truncPyramid"].BaseVertexLocation;
	mAllRitems.push_back(std::move(truncTopLItem));


	// Right tower Cube
	auto cubeTowerRItem = std::make_unique<RenderItem>();
	XMMATRIX cubeTowerRWorld = XMMatrixScaling(3.0f, 6.0f, 4.0f) * XMMatrixTranslation(6.5f, 3.0f, 4.0f);
	XMStoreFloat4x4(&cubeTowerRItem->World, cubeTowerRWorld);
	cubeTowerRItem->TexTransform = MathHelper::Identity4x4();
	cubeTowerRItem->ObjCBIndex = objCBIndex++;
	cubeTowerRItem->Mat = mMaterials["coneMat"].get();
	cubeTowerRItem->Geo = mGeometries["shapeGeo"].get();
	cubeTowerRItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	cubeTowerRItem->IndexCount = cubeTowerRItem->Geo->DrawArgs["box"].IndexCount;
	cubeTowerRItem->StartIndexLocation = cubeTowerRItem->Geo->DrawArgs["box"].StartIndexLocation;
	cubeTowerRItem->BaseVertexLocation = cubeTowerRItem->Geo->DrawArgs["box"].BaseVertexLocation;
	mAllRitems.push_back(std::move(cubeTowerRItem));

	// Right tower Top
	// TruncacatedPyramid 
	auto truncTopRItem = std::make_unique<RenderItem>();
	XMMATRIX truncTopRWorld = XMMatrixScaling(3.0f, 3.0f, 4.0f) * XMMatrixTranslation(6.5f, 7.5f, 4.0f);
	XMStoreFloat4x4(&truncTopRItem->World, truncTopRWorld);
	truncTopRItem->TexTransform = MathHelper::Identity4x4();
	truncTopRItem->ObjCBIndex = objCBIndex++;
	truncTopRItem->Mat = mMaterials["coneMat"].get();
	truncTopRItem->Geo = mGeometries["shapeGeo"].get();
	truncTopRItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	truncTopRItem->IndexCount = truncTopRItem->Geo->DrawArgs["truncPyramid"].IndexCount;
	truncTopRItem->StartIndexLocation = truncTopRItem->Geo->DrawArgs["truncPyramid"].StartIndexLocation;
	truncTopRItem->BaseVertexLocation = truncTopRItem->Geo->DrawArgs["truncPyramid"].BaseVertexLocation;
	mAllRitems.push_back(std::move(truncTopRItem));



	// Right House long Cube
	auto cubeHouseLItem = std::make_unique<RenderItem>();
	XMMATRIX cubeHouseLWorld = XMMatrixScaling(2.0f, 2.0f, 5.0f) * XMMatrixTranslation(7.5f, 1.0f, -6.5f);
	XMStoreFloat4x4(&cubeHouseLItem->World, cubeHouseLWorld);
	cubeHouseLItem->TexTransform = MathHelper::Identity4x4();
	cubeHouseLItem->ObjCBIndex = objCBIndex++;
	cubeHouseLItem->Mat = mMaterials["coneMat"].get();
	cubeHouseLItem->Geo = mGeometries["shapeGeo"].get();
	cubeHouseLItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	cubeHouseLItem->IndexCount = cubeHouseLItem->Geo->DrawArgs["box"].IndexCount;
	cubeHouseLItem->StartIndexLocation = cubeHouseLItem->Geo->DrawArgs["box"].StartIndexLocation;
	cubeHouseLItem->BaseVertexLocation = cubeHouseLItem->Geo->DrawArgs["box"].BaseVertexLocation;
	mAllRitems.push_back(std::move(cubeHouseLItem));


	// Primitive Examples
	// Cone 
	auto coneItem = std::make_unique<RenderItem>();
	XMMATRIX coneWorld = XMMatrixRotationY(0.0f * (XM_PI / 180.0f)) * XMMatrixScaling(1.5f, 2.0f, 1.5f) * XMMatrixTranslation(-5.0f, 1.0f, -4.0f);
	XMStoreFloat4x4(&coneItem->World, coneWorld);
	coneItem->TexTransform = MathHelper::Identity4x4();
	coneItem->ObjCBIndex = objCBIndex++;
	coneItem->Mat = mMaterials["coneMat"].get();
	coneItem->Geo = mGeometries["shapeGeo"].get();
	coneItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	coneItem->IndexCount = coneItem->Geo->DrawArgs["cone"].IndexCount;
	coneItem->StartIndexLocation = coneItem->Geo->DrawArgs["cone"].StartIndexLocation;
	coneItem->BaseVertexLocation = coneItem->Geo->DrawArgs["cone"].BaseVertexLocation;
	mAllRitems.push_back(std::move(coneItem));

	// Wedge
	auto wedgeItem = std::make_unique<RenderItem>();
	XMMATRIX wedgeWorld  = XMMatrixScaling(1.5f, 2.0f, 1.5f) * XMMatrixTranslation(-3.0f, 1.0f, -4.0f);
	XMStoreFloat4x4(&wedgeItem->World, wedgeWorld);
	wedgeItem->TexTransform = MathHelper::Identity4x4();
	wedgeItem->ObjCBIndex = objCBIndex++;
	wedgeItem->Mat = mMaterials["coneMat"].get();
	wedgeItem->Geo = mGeometries["shapeGeo"].get();
	wedgeItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	wedgeItem->IndexCount = wedgeItem->Geo->DrawArgs["wedge"].IndexCount;
	wedgeItem->StartIndexLocation = wedgeItem->Geo->DrawArgs["wedge"].StartIndexLocation;
	wedgeItem->BaseVertexLocation = wedgeItem->Geo->DrawArgs["wedge"].BaseVertexLocation;
	mAllRitems.push_back(std::move(wedgeItem));

	// Pyramid
	auto pyramidItem = std::make_unique<RenderItem>();
	XMMATRIX pyramidWorld = XMMatrixScaling(1.5f, 2.0f, 1.5f) * XMMatrixTranslation(-1.0f, 1.0f, -4.0f);
	XMStoreFloat4x4(&pyramidItem->World, pyramidWorld);
	pyramidItem->TexTransform = MathHelper::Identity4x4();
	pyramidItem->ObjCBIndex = objCBIndex++;
	pyramidItem->Mat = mMaterials["coneMat"].get();
	pyramidItem->Geo = mGeometries["shapeGeo"].get();
	pyramidItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	pyramidItem->IndexCount = pyramidItem->Geo->DrawArgs["pyramid"].IndexCount;
	pyramidItem->StartIndexLocation = pyramidItem->Geo->DrawArgs["pyramid"].StartIndexLocation;
	pyramidItem->BaseVertexLocation = pyramidItem->Geo->DrawArgs["pyramid"].BaseVertexLocation;
	mAllRitems.push_back(std::move(pyramidItem));

	// Truncated pyramid
	auto truncPyramidItem = std::make_unique<RenderItem>();
	XMMATRIX truncPyramidWorld = XMMatrixScaling(1.5f, 2.0f, 1.5f) * XMMatrixTranslation(1.0f, 1.0f, -4.0f);
	XMStoreFloat4x4(&truncPyramidItem->World, truncPyramidWorld);
	truncPyramidItem->TexTransform = MathHelper::Identity4x4();
	truncPyramidItem->ObjCBIndex = objCBIndex++;
	truncPyramidItem->Mat = mMaterials["coneMat"].get();
	truncPyramidItem->Geo = mGeometries["shapeGeo"].get();
	truncPyramidItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	truncPyramidItem->IndexCount = truncPyramidItem->Geo->DrawArgs["truncPyramid"].IndexCount;
	truncPyramidItem->StartIndexLocation = truncPyramidItem->Geo->DrawArgs["truncPyramid"].StartIndexLocation;
	truncPyramidItem->BaseVertexLocation = truncPyramidItem->Geo->DrawArgs["truncPyramid"].BaseVertexLocation;
	mAllRitems.push_back(std::move(truncPyramidItem));

	// Triangular prism
	auto triangularPrismItem = std::make_unique<RenderItem>();
	XMMATRIX triangularPrismWorld = XMMatrixScaling(1.5f, 2.0f, 1.5f) * XMMatrixTranslation(3.0f, 1.0f, -4.0f);
	XMStoreFloat4x4(&triangularPrismItem->World, triangularPrismWorld);
	triangularPrismItem->TexTransform = MathHelper::Identity4x4();
	triangularPrismItem->ObjCBIndex = objCBIndex++;
	triangularPrismItem->Mat = mMaterials["coneMat"].get();
	triangularPrismItem->Geo = mGeometries["shapeGeo"].get();
	triangularPrismItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	triangularPrismItem->IndexCount = triangularPrismItem->Geo->DrawArgs["triangularPrism"].IndexCount;
	triangularPrismItem->StartIndexLocation = triangularPrismItem->Geo->DrawArgs["triangularPrism"].StartIndexLocation;
	triangularPrismItem->BaseVertexLocation = triangularPrismItem->Geo->DrawArgs["triangularPrism"].BaseVertexLocation;
	mAllRitems.push_back(std::move(triangularPrismItem));

	// Tetrahedron
	auto tetrahedronItem = std::make_unique<RenderItem>();
	XMMATRIX tetrahedronWorld = XMMatrixScaling(1.5f, 2.0f, 1.5f) * XMMatrixTranslation(5.0f, 1.0f, -4.0f);
	XMStoreFloat4x4(&tetrahedronItem->World, tetrahedronWorld);
	tetrahedronItem->TexTransform = MathHelper::Identity4x4();
	tetrahedronItem->ObjCBIndex = objCBIndex++;
	tetrahedronItem->Mat = mMaterials["coneMat"].get();
	tetrahedronItem->Geo = mGeometries["shapeGeo"].get();
	tetrahedronItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	tetrahedronItem->IndexCount = tetrahedronItem->Geo->DrawArgs["tetrahedron"].IndexCount;
	tetrahedronItem->StartIndexLocation = tetrahedronItem->Geo->DrawArgs["tetrahedron"].StartIndexLocation;
	tetrahedronItem->BaseVertexLocation = tetrahedronItem->Geo->DrawArgs["tetrahedron"].BaseVertexLocation;
	mAllRitems.push_back(std::move(tetrahedronItem));

	// All the render items are opaque.
	for(auto& e : mAllRitems)
		mOpaqueRitems.push_back(e.get());
}

void LitColumnsApp::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
{
    UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
    UINT matCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(MaterialConstants));
 
	auto objectCB = mCurrFrameResource->ObjectCB->Resource();
	auto matCB = mCurrFrameResource->MaterialCB->Resource();

    // For each render item...
    for(size_t i = 0; i < ritems.size(); ++i)
    {
        auto ri = ritems[i];

        cmdList->IASetVertexBuffers(0, 1, &ri->Geo->VertexBufferView());
        cmdList->IASetIndexBuffer(&ri->Geo->IndexBufferView());
        cmdList->IASetPrimitiveTopology(ri->PrimitiveType);

        D3D12_GPU_VIRTUAL_ADDRESS objCBAddress = objectCB->GetGPUVirtualAddress() + ri->ObjCBIndex*objCBByteSize;
		D3D12_GPU_VIRTUAL_ADDRESS matCBAddress = matCB->GetGPUVirtualAddress() + ri->Mat->MatCBIndex*matCBByteSize;

        cmdList->SetGraphicsRootConstantBufferView(0, objCBAddress);
		cmdList->SetGraphicsRootConstantBufferView(1, matCBAddress);

        cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
    }
}
