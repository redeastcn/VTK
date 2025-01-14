// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
#include "vtkAnariVolumeMapperNode.h"
#include "vtkAnariProfiling.h"
#include "vtkAnariRendererNode.h"

#include "vtkAbstractVolumeMapper.h"
#include "vtkArrayDispatch.h"
#include "vtkCellDataToPointData.h"
#include "vtkColorTransferFunction.h"
#include "vtkDataArray.h"
#include "vtkDataArrayRange.h"
#include "vtkFloatArray.h"
#include "vtkImageData.h"
#include "vtkLogger.h"
#include "vtkObjectFactory.h"
#include "vtkPiecewiseFunction.h"
#include "vtkPointData.h"
#include "vtkTypeInt32Array.h"
#include "vtkTypeUInt8Array.h"
#include "vtkVolume.h"
#include "vtkVolumeNode.h"
#include "vtkVolumeProperty.h"

#include <algorithm>

#include <anari/anari_cpp.hpp>
#include <anari/anari_cpp/ext/std.h>

using vec3 = anari::std_types::vec3;

//============================================================================
namespace anari_structured
{
VTK_ABI_NAMESPACE_BEGIN
struct TransferFunction
{
  TransferFunction()
    : color()
    , opacity()
    , valueRange{ 0, 1 }
  {
  }

  std::vector<vec3> color;
  std::vector<float> opacity;
  float valueRange[2];
};

struct StructuredRegularSpatialFieldDataWorker
{
  StructuredRegularSpatialFieldDataWorker()
    : AnariDevice(nullptr)
    , AnariSpatialField(nullptr)
    , Dim(nullptr)
  {
  }

  //------------------------------------------------------------------------------
  template <typename ScalarArray>
  void operator()(ScalarArray* scalars)
  {
    if (this->AnariDevice == nullptr || this->AnariSpatialField == nullptr || this->Dim == nullptr)
    {
      vtkLogF(ERROR, "[ANARI::ERROR] %s\n",
        "StructuredRegularSpatialFieldDataWorker not properly initialized");
      return;
    }

    VTK_ASSUME(scalars->GetNumberOfComponents() == 1);
    const auto scalarRange = vtk::DataArrayValueRange<1>(scalars);

    auto dataArray =
      anari::newArray3D(this->AnariDevice, ANARI_FLOAT32, this->Dim[0], this->Dim[1], this->Dim[2]);
    {
      auto dataArrayPtr = anari::map<float>(this->AnariDevice, dataArray);
      int i = 0;

      for (auto val : scalarRange)
      {
        dataArrayPtr[i++] = static_cast<float>(val);
      }

      anari::unmap(this->AnariDevice, dataArray);
    }

    anari::setAndReleaseParameter(this->AnariDevice, this->AnariSpatialField, "data", dataArray);
  }

  anari::Device AnariDevice;
  anari::SpatialField AnariSpatialField;
  int* Dim;
};
VTK_ABI_NAMESPACE_END
} // namespace: anari_structured

VTK_ABI_NAMESPACE_BEGIN

class vtkAnariVolumeMapperNodeInternals
{
public:
  vtkAnariVolumeMapperNodeInternals(vtkAnariVolumeMapperNode*);
  ~vtkAnariVolumeMapperNodeInternals() = default;

  void UpdateTransferFunction(vtkVolume* const, const double, const double);
  vtkDataArray* ConvertScalarData(vtkDataArray* const, const int, const int);

  void StageVolume(const bool);

  vtkTimeStamp BuildTime;
  vtkTimeStamp PropertyTime;

  std::string LastArrayName;
  int LastArrayComponent;

  vtkAnariVolumeMapperNode* Owner;
  vtkAnariRendererNode* AnariRendererNode;
  anari::Volume AnariVolume;
  std::unique_ptr<anari_structured::TransferFunction> TransferFunction;
};

//----------------------------------------------------------------------------
vtkAnariVolumeMapperNodeInternals::vtkAnariVolumeMapperNodeInternals(
  vtkAnariVolumeMapperNode* owner)
  : BuildTime()
  , PropertyTime()
  , LastArrayName("")
  , LastArrayComponent(-2)
  , Owner(owner)
  , AnariRendererNode(nullptr)
  , AnariVolume(nullptr)
  , TransferFunction(nullptr)
{
}

//----------------------------------------------------------------------------
void vtkAnariVolumeMapperNodeInternals::StageVolume(const bool changed)
{
  vtkAnariProfiling startProfiling(
    "vtkAnariVolumeMapperNode::RenderVolumes", vtkAnariProfiling::GREEN);

  if (this->AnariRendererNode != nullptr)
  {
    this->AnariRendererNode->AddVolume(this->AnariVolume, changed);
  }
}

//------------------------------------------------------------------------------
void vtkAnariVolumeMapperNodeInternals::UpdateTransferFunction(
  vtkVolume* const vtkVol, const double low, const double high)
{
  this->TransferFunction.reset(new anari_structured::TransferFunction());
  vtkVolumeProperty* volProperty = vtkVol->GetProperty();
  const int transferFunctionMode = volProperty->GetTransferFunctionMode();

  if (transferFunctionMode == vtkVolumeProperty::TF_2D)
  {
    vtkWarningWithObjectMacro(
      this->Owner, << "ANARI currently doesn't support 2D transfer functions. "
                   << "Using default RGB and Scalar transfer functions.");
  }

  if (volProperty->HasGradientOpacity())
  {
    vtkWarningWithObjectMacro(this->Owner, << "ANARI currently doesn't support gradient opacity");
  }

  vtkColorTransferFunction* colorTF = volProperty->GetRGBTransferFunction(0);
  vtkPiecewiseFunction* opacityTF = volProperty->GetScalarOpacity(0);

  // Value Range
  double tfRange[2] = { 0, -1 };

  if (transferFunctionMode == vtkVolumeProperty::TF_1D)
  {
    double* tfRangePtr = colorTF->GetRange();
    tfRange[0] = tfRangePtr[0];
    tfRange[1] = tfRangePtr[1];
  }

  if (tfRange[1] <= tfRange[0])
  {
    tfRange[0] = low;
    tfRange[1] = high;
  }

  this->TransferFunction->valueRange[0] = static_cast<float>(tfRange[0]);
  this->TransferFunction->valueRange[1] = static_cast<float>(tfRange[1]);

  // Opacity
  int opacitySize = this->Owner->GetOpacitySize();
  this->TransferFunction->opacity.resize(opacitySize);
  opacityTF->GetTable(tfRange[0], tfRange[1], opacitySize, &this->TransferFunction->opacity[0]);

  // Color
  int colorSize = this->Owner->GetColorSize();
  float colorArray[colorSize * 3];
  colorTF->GetTable(tfRange[0], tfRange[1], colorSize, &colorArray[0]);

  for (int i = 0, j = 0; i < colorSize; i++, j += 3)
  {
    this->TransferFunction->color.emplace_back(
      vec3{ colorArray[j], colorArray[j + 1], colorArray[j + 2] });
  }
}

//------------------------------------------------------------------------------
vtkDataArray* vtkAnariVolumeMapperNodeInternals::ConvertScalarData(
  vtkDataArray* const scalarData, const int vectorComponent, const int vectorMode)
{
  int numComponents = scalarData->GetNumberOfComponents();
  const vtkIdType numTuples = scalarData->GetNumberOfTuples();
  vtkDataArray* scalarDataOut = nullptr;

  if (numComponents > 1)
  {
    scalarDataOut = scalarData->NewInstance();
    scalarDataOut->SetNumberOfComponents(1);
    scalarDataOut->SetNumberOfTuples(numTuples);

    if (vectorMode != vtkColorTransferFunction::MAGNITUDE)
    {
      scalarDataOut->CopyComponent(0, scalarData, vectorComponent);
    }
    else
    {
      for (vtkIdType t = 0; t < numTuples; t++)
      {
        scalarDataOut->SetTuple1(t, vtkMath::Norm(scalarData->GetTuple3(t)));
      }
    }
  }

  return scalarDataOut;
}

//============================================================================
vtkStandardNewMacro(vtkAnariVolumeMapperNode);

//----------------------------------------------------------------------------
vtkAnariVolumeMapperNode::vtkAnariVolumeMapperNode()
  : ColorSize(128)
  , OpacitySize(128)
{
  this->Internal = new vtkAnariVolumeMapperNodeInternals(this);
}

//----------------------------------------------------------------------------
vtkAnariVolumeMapperNode::~vtkAnariVolumeMapperNode()
{
  delete this->Internal;
}

//----------------------------------------------------------------------------
void vtkAnariVolumeMapperNode::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
}

//----------------------------------------------------------------------------
void vtkAnariVolumeMapperNode::Render(bool prepass)
{
  vtkAnariProfiling startProfiling("vtkAnariVolumeMapperNode::Render", vtkAnariProfiling::GREEN);

  if (prepass)
  {
    vtkVolumeNode* volNode = vtkVolumeNode::SafeDownCast(this->Parent);
    vtkVolume* vol = vtkVolume::SafeDownCast(volNode->GetRenderable());

    if (vol->GetVisibility() == false)
    {
      vtkDebugMacro(<< "Volume visibility off");
      return;
    }

    vtkVolumeProperty* const volumeProperty = vol->GetProperty();

    if (!volumeProperty)
    {
      // this is OK, happens in paraview client side for instance
      vtkDebugMacro(<< "Volume doesn't have property set");
      return;
    }

    vtkAbstractVolumeMapper* mapper = vtkAbstractVolumeMapper::SafeDownCast(this->GetRenderable());

    // make sure that we have scalar input and update the scalar input
    if (mapper->GetDataSetInput() == nullptr)
    {
      // OK - PV cli/srv for instance vtkErrorMacro("VolumeMapper had no input!");
      vtkDebugMacro(<< "No scalar input for the Volume");
      return;
    }

    mapper->GetInputAlgorithm()->UpdateInformation();
    mapper->GetInputAlgorithm()->Update();
    vtkDataSet* dataSet = mapper->GetDataSetInput();
    vtkImageData* data = vtkImageData::SafeDownCast(dataSet);

    if (!data)
    {
      vtkDebugMacro("VolumeMapper's Input has no data!");
      return;
    }

    int fieldAssociation;
    vtkDataArray* sa = vtkDataArray::SafeDownCast(this->GetArrayToProcess(data, fieldAssociation));

    if (!sa)
    {
      vtkErrorMacro("VolumeMapper's Input has no scalar array!");
      return;
    }

    const int vectorComponent = volumeProperty->GetRGBTransferFunction()->GetVectorComponent();
    const int vectorMode = volumeProperty->GetRGBTransferFunction()->GetVectorMode();

    vtkDataArray* sca = this->Internal->ConvertScalarData(sa, vectorComponent, vectorMode);

    if (sca != nullptr)
    {
      sa = sca;
    }

    this->Internal->AnariRendererNode =
      static_cast<vtkAnariRendererNode*>(this->GetFirstAncestorOfType("vtkAnariRendererNode"));
    vtkMTimeType inTime = volNode->GetMTime();
    auto anariDevice = this->Internal->AnariRendererNode->GetAnariDevice();

    //
    // Create ANARI Volume
    //
    bool isNewVolume = false;

    if (this->Internal->AnariVolume == nullptr)
    {
      isNewVolume = true;
      this->Internal->AnariVolume =
        anari::newObject<anari::Volume>(anariDevice, "transferFunction1D");
    }

    auto anariVolume = this->Internal->AnariVolume;

    if (mapper->GetDataSetInput()->GetMTime() > this->Internal->BuildTime ||
      this->Internal->LastArrayName != mapper->GetArrayName() ||
      this->Internal->LastArrayComponent != vectorComponent)
    {
      this->Internal->LastArrayName = mapper->GetArrayName();
      this->Internal->LastArrayComponent = vectorComponent;

      // Spatial Field
      auto anariSpatialField =
        anari::newObject<anari::SpatialField>(anariDevice, "structuredRegular");

      double origin[3];
      data->GetOrigin(origin);

      vec3 gridOrigin = { static_cast<float>(origin[0]), static_cast<float>(origin[1]),
        static_cast<float>(origin[2]) };
      anari::setParameter(anariDevice, anariSpatialField, "origin", gridOrigin);

      double spacing[3];
      data->GetSpacing(spacing);
      vec3 gridSpacing = { static_cast<float>(spacing[0]), static_cast<float>(spacing[1]),
        static_cast<float>(spacing[2]) };

      anari::setParameter(anariDevice, anariSpatialField, "spacing", gridSpacing);

      // Filter
      const int filterType = vol->GetProperty()->GetInterpolationType();

      if (filterType == VTK_LINEAR_INTERPOLATION)
      {
        anari::setParameter(anariDevice, anariSpatialField, "filter", "linear");
      }
      else if (filterType == VTK_NEAREST_INTERPOLATION)
      {
        anari::setParameter(anariDevice, anariSpatialField, "filter", "nearest");
      }
      else if (filterType == VTK_CUBIC_INTERPOLATION)
      {
        vtkWarningMacro(
          << "ANARI currently doesn't support cubic interpolation, using default value.");
      }
      else
      {
        vtkWarningMacro(<< "ANARI currently only supports linear and nearest interpolation, using "
                           "default value.");
      }

      int dim[3];
      data->GetDimensions(dim);

      if (fieldAssociation == vtkDataObject::FIELD_ASSOCIATION_CELLS)
      {
        dim[0] -= 1;
        dim[1] -= 1;
        dim[2] -= 1;
      }

      vtkDebugMacro(<< "Volume Dimensions: " << dim[0] << "x" << dim[1] << "x" << dim[2]);

      // Create the actual field values for the 3D grid; the scalars are assumed to be
      // vertex centered.
      anari_structured::StructuredRegularSpatialFieldDataWorker worker;
      worker.AnariDevice = anariDevice;
      worker.AnariSpatialField = anariSpatialField;
      worker.Dim = dim;

      using Dispatcher = vtkArrayDispatch::DispatchByValueType<vtkTypeList::Create<double, float,
        int, unsigned int, char, unsigned char, unsigned short, short>>;

      if (!Dispatcher::Execute(sa, worker))
      {
        worker(sa);
      }

      anari::commitParameters(anariDevice, anariSpatialField);
      anari::setAndReleaseParameter(anariDevice, anariVolume, "field", anariSpatialField);
      anari::commitParameters(anariDevice, anariVolume);
    }

    if (isNewVolume || volumeProperty->GetMTime() > this->Internal->PropertyTime ||
      mapper->GetDataSetInput()->GetMTime() > this->Internal->BuildTime)
    {
      // Transfer Function
      double scalarRange[2];
      sa->GetRange(scalarRange);

      this->Internal->UpdateTransferFunction(vol, scalarRange[0], scalarRange[1]);
      anari_structured::TransferFunction* transferFunction = this->Internal->TransferFunction.get();

      anariSetParameter(
        anariDevice, anariVolume, "valueRange", ANARI_FLOAT32_BOX1, transferFunction->valueRange);

      auto array1DColor = anari::newArray1D(
        anariDevice, transferFunction->color.data(), transferFunction->color.size());
      anari::setAndReleaseParameter(anariDevice, anariVolume, "color", array1DColor);

      auto array1DOpacity = anari::newArray1D(
        anariDevice, transferFunction->opacity.data(), transferFunction->opacity.size());
      anari::setAndReleaseParameter(anariDevice, anariVolume, "opacity", array1DOpacity);

      anari::commitParameters(anariDevice, anariVolume);
      this->Internal->PropertyTime.Modified();
    }

    if (sca)
    {
      sca->Delete();
    }

    this->Internal->StageVolume(isNewVolume);
    this->RenderTime = volNode->GetMTime();
    this->Internal->BuildTime.Modified();
  }
}

VTK_ABI_NAMESPACE_END
