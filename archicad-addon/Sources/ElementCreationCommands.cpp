#include "ElementCreationCommands.hpp"
#include "ObjectState.hpp"
#include "MigrationHelper.hpp"
#include "NotificationCommands.hpp"
#include <cmath>

CreateElementsCommandBase::CreateElementsCommandBase (const GS::String& commandNameIn, API_ElemTypeID elemTypeIDIn, const GS::String& arrayFieldNameIn)
    : CommandBase (CommonSchema::Used)
    , commandName (commandNameIn)
    , elemTypeID (elemTypeIDIn)
    , arrayFieldName (arrayFieldNameIn)
{
}

GS::String CreateElementsCommandBase::GetName () const
{
    return commandName;
}

GS::Optional<GS::UniString> CreateElementsCommandBase::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elements": {
                "$ref": "#/Elements"
            }
        },
        "additionalProperties": false,
        "required": [
            "elements"
        ]
    })";
}

GS::ObjectState	CreateElementsCommandBase::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> dataArray;
    parameters.Get (arrayFieldName, dataArray);

    GS::ObjectState response;
    const auto& elements = response.AddList<GS::ObjectState> ("elements");

    const GS::UniString elemTypeName = GetElementTypeNonLocalizedName (elemTypeID);
    const Stories stories = GetStories ();

    API_NotifyElementType notification = {};
    notification.notifID = APINotifyElement_BeginEvents;
    AddElementNotificationClientCommand::ElementEventHandlerProc (&notification);

    ACAPI_CallUndoableCommand ("Create " + elemTypeName, [&] () -> GSErrCode {
        API_Element element = {};
        API_ElementMemo memo = {};
        const GS::OnExit guard ([&memo] () { ACAPI_DisposeElemMemoHdls (&memo); });

#ifdef ServerMainVers_2600
        element.header.type   = elemTypeID;
#else
        element.header.typeID = elemTypeID;
#endif
        GSErrCode err = ACAPI_Element_GetDefaults (&element, &memo);

        bool savedAutoTextFlag;
        ACAPI_AutoText_GetAutoTextFlag (&savedAutoTextFlag);
        bool setAutoTextFlag = false;
        ACAPI_AutoText_ChangeAutoTextFlag (&setAutoTextFlag);

        for (const GS::ObjectState& data : dataArray) {
            auto os = SetTypeSpecificParameters (element, memo, stories, data);
            if (os.HasValue ()) {
                elements (*os);
                continue;
            }

            err = ACAPI_Element_Create (&element, &memo);
            if (err != NoError) {
                elements (CreateErrorResponse (err, "Failed to create new " + elemTypeName));
                continue;
            }

            notification = {};
            notification.notifID = APINotifyElement_New;
            notification.elemHead = element.header;
            AddElementNotificationClientCommand::ElementEventHandlerProc (&notification);

            elements (CreateElementIdObjectState (element.header.guid));
        }

        ACAPI_AutoText_ChangeAutoTextFlag (&savedAutoTextFlag);

        return NoError;
    });

    notification = {};
    notification.notifID = APINotifyElement_EndEvents;
    AddElementNotificationClientCommand::ElementEventHandlerProc (&notification);

    return response;
}

CreateColumnsCommand::CreateColumnsCommand () :
    CreateElementsCommandBase ("CreateColumns", API_ColumnID, "columnsData")
{
}

GS::Optional<GS::UniString> CreateColumnsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "columnsData": {
                "type": "array",
                "description": "Array of data to create Columns.",
                "items": {
                    "type": "object",
                    "description": "The parameters of the new Column.",
                    "properties": {
                        "coordinates": {
                            "type": "object",
                            "description" : "3D coordinate.",
                            "properties" : {
                                "x": {
                                    "type": "number",
                                    "description" : "X value of the coordinate."
                                },
                                "y" : {
                                    "type": "number",
                                    "description" : "Y value of the coordinate."
                                },
                                "z" : {
                                    "type": "number",
                                    "description" : "Z value of the coordinate."
                                }
                            },
                            "additionalProperties": false,
                            "required" : [
                                "x",
                                "y",
                                "z"
                            ]
                        }
                    },
                    "additionalProperties": false,
                    "required" : [
                        "coordinates"
                    ]
                }
            }
        },
        "additionalProperties": false,
        "required": [
            "columnsData"
        ]
    })";
}

GS::Optional<GS::ObjectState> CreateColumnsCommand::SetTypeSpecificParameters (API_Element& element, API_ElementMemo&, const Stories& stories, const GS::ObjectState& parameters) const
{
    GS::ObjectState coordinates;
    parameters.Get ("coordinates", coordinates);
    API_Coord3D apiCoordinate = Get3DCoordinateFromObjectState (coordinates);

    const auto floorIndexAndOffset = GetFloorIndexAndOffset (apiCoordinate.z, stories);
    element.header.floorInd = floorIndexAndOffset.first;
    element.column.bottomOffset = floorIndexAndOffset.second;
    element.column.origoPos.x = apiCoordinate.x;
    element.column.origoPos.y = apiCoordinate.y;

    return {};
}

CreateSlabsCommand::CreateSlabsCommand () :
    CreateElementsCommandBase ("CreateSlabs", API_SlabID, "slabsData")
{
}

GS::Optional<GS::UniString> CreateSlabsCommand::GetInputParametersSchema () const
{
    return R"({
    "type": "object",
    "properties": {
        "slabsData": {
            "type": "array",
            "description": "Array of data to create Slabs.",
            "items": {
                "type": "object",
                "description" : "The parameters of the new Slab.",
                "properties" : {
                    "level": {
                        "type": "number",
                        "description" : "The Z coordinate value of the reference line of the slab."	
                    },
                    "polygonCoordinates": { 
                        "type": "array",
                        "description": "The 2D coordinates of the edge of the slab.",
                        "items": {
                            "$ref": "#/Coordinate2D"
                        },
                        "minItems": 3
                    },
                    "polygonArcs": {
                        "type": "array",
                        "description": "Polygon outline arcs of the slab.",
                        "items": {
                            "$ref": "#/PolyArc"
                        }
                    },
                    "holes" : {
                        "$ref": "#/Holes2D"
                    }    
                },
                "additionalProperties": false,
                "required" : [
                    "level",
                    "polygonCoordinates"
                ]
            }
        }
    },
    "additionalProperties": false,
    "required": [
        "slabsData"
    ]
})";
}

static GS::Array<API_PolyArc> GetPolyArcs (const GS::Array<GS::ObjectState>& arcs, Int32 iStart)
{
    GS::Array<API_PolyArc> polyArcs;
    for (const GS::ObjectState& arc : arcs) {
        API_PolyArc polyArc = {};
        if (arc.Get ("begIndex", polyArc.begIndex) &&
            arc.Get ("endIndex", polyArc.endIndex) &&
            arc.Get ("arcAngle", polyArc.arcAngle)) {
            polyArc.begIndex += iStart;
            polyArc.endIndex += iStart;
            polyArcs.Push (polyArc);
        }
    }
    return polyArcs;
}

static void AddPolyToMemo (const GS::Array<GS::ObjectState>& coords,
                           const GS::Array<GS::ObjectState>& arcs,
                           Int32&                            iCoord,
                           Int32&                            iArc,
                           Int32&                            iPends,
                           API_ElementMemo&                  memo,
                           const API_EdgeTrimID*             edgeTrimSideType = nullptr,
                           const API_OverriddenAttribute*    sideMat = nullptr)
{
    Int32 iStart = iCoord;
    for (const GS::ObjectState& coord : coords) {
        (*memo.coords)[iCoord] = Get2DCoordinateFromObjectState (coord);
        if (edgeTrimSideType != nullptr) {
            (*memo.edgeTrims)[iCoord].sideType = *edgeTrimSideType;
        }
        if (sideMat != nullptr) {
            memo.sideMaterials[iCoord] = *sideMat;
        }
        if (memo.meshPolyZ != nullptr) {
            coord.Get ("z", (*memo.meshPolyZ)[iCoord]);
        }
        ++iCoord;
    }
    (*memo.coords)[iCoord] = (*memo.coords)[iStart];
    (*memo.pends)[iPends++] = iCoord;
    if (edgeTrimSideType != nullptr) {
        (*memo.edgeTrims)[iCoord].sideType = (*memo.edgeTrims)[iStart].sideType;
        (*memo.edgeTrims)[iCoord].sideAngle = (*memo.edgeTrims)[iStart].sideAngle;
    }
    if (sideMat != nullptr) {
        memo.sideMaterials[iCoord] = memo.sideMaterials[iStart];
    }
    ++iCoord;

    const GS::Array<API_PolyArc> polyArcs = GetPolyArcs (arcs, iStart);
    for (const API_PolyArc& a : polyArcs) {
        (*memo.parcs)[iArc++] = a;
    }
}

GS::Optional<GS::ObjectState> CreateSlabsCommand::SetTypeSpecificParameters (API_Element& element, API_ElementMemo& memo, const Stories& stories, const GS::ObjectState& parameters) const
{
    double inputLevel = 0.0;
    parameters.Get ("level", inputLevel);
    const auto floorIndexAndOffset = GetFloorIndexAndOffset (inputLevel, stories);
    element.header.floorInd = floorIndexAndOffset.first;
    element.slab.level = floorIndexAndOffset.second;

    GS::Array<GS::ObjectState> polygonCoordinates;
    GS::Array<GS::ObjectState> polygonArcs;
    GS::Array<GS::ObjectState> holes;
    parameters.Get ("polygonCoordinates", polygonCoordinates);
    parameters.Get ("polygonArcs", polygonArcs);
    parameters.Get ("holes", holes);
    if (IsSame2DCoordinate (polygonCoordinates.GetFirst (), polygonCoordinates.GetLast ())) {
        polygonCoordinates.Pop ();
    }
    element.slab.poly.nCoords	= polygonCoordinates.GetSize() + 1;
    element.slab.poly.nSubPolys	= 1;
    element.slab.poly.nArcs		= polygonArcs.GetSize ();

    for (const GS::ObjectState& hole : holes) {
        GS::Array<GS::ObjectState> holePolygonOutline;
        GS::Array<GS::ObjectState> holePolygonArcs;
        if (GetHoleGeometry (hole, holePolygonOutline, holePolygonArcs)) {
            element.slab.poly.nCoords += holePolygonOutline.GetSize () + 1;
            ++element.slab.poly.nSubPolys;
            element.slab.poly.nArcs += holePolygonArcs.GetSize ();
        }
    }

    memo.coords = reinterpret_cast<API_Coord**> (BMAllocateHandle ((element.slab.poly.nCoords + 1) * sizeof (API_Coord), ALLOCATE_CLEAR, 0));
    memo.edgeTrims = reinterpret_cast<API_EdgeTrim**> (BMAllocateHandle ((element.slab.poly.nCoords + 1) * sizeof (API_EdgeTrim), ALLOCATE_CLEAR, 0));
    memo.sideMaterials = reinterpret_cast<API_OverriddenAttribute*> (BMAllocatePtr ((element.slab.poly.nCoords + 1) * sizeof (API_OverriddenAttribute), ALLOCATE_CLEAR, 0));
    memo.pends = reinterpret_cast<Int32**> (BMAllocateHandle ((element.slab.poly.nSubPolys + 1) * sizeof (Int32), ALLOCATE_CLEAR, 0));
    memo.parcs = reinterpret_cast<API_PolyArc**> (BMAllocateHandle (element.slab.poly.nArcs * sizeof (API_PolyArc), ALLOCATE_CLEAR, 0));

    Int32 iCoord = 1;
    Int32 iArc = 0;
    Int32 iPends = 1;
    const API_EdgeTrimID edgeTrimSideType = APIEdgeTrim_Vertical; // Only vertical trim is supported yet by my code
    AddPolyToMemo(polygonCoordinates,
                  polygonArcs,
                  iCoord,
                  iArc,
                  iPends,
                  memo,
                  &edgeTrimSideType,
                  &element.slab.sideMat);

    for (const GS::ObjectState& hole : holes) {
        GS::Array<GS::ObjectState> holePolygonOutline;
        GS::Array<GS::ObjectState> holePolygonArcs;
        if (GetHoleGeometry (hole, holePolygonOutline, holePolygonArcs)) {
            AddPolyToMemo (holePolygonOutline,
                          holePolygonArcs,
                          iCoord,
                          iArc,
                          iPends,
                          memo,
                          &edgeTrimSideType,
                          &element.slab.sideMat);
        }
    }

    return {};
}

CreateZonesCommand::CreateZonesCommand () :
    CreateElementsCommandBase ("CreateZones", API_ZoneID, "zonesData")
{
}

GS::Optional<GS::UniString> CreateZonesCommand::GetInputParametersSchema () const
{
    return R"({
    "type": "object",
    "properties": {
        "zonesData": {
            "type": "array",
            "description": "Array of data to create Zones.",
            "items": {
                "type": "object",
                "description" : "The parameters of the new Zone.",
                "properties" : {
                    "floorIndex": {
                        "type": "number"
                    },
                    "name": {
                        "type": "string",
                        "description" : "Name of the zone."
                    },
                    "numberStr": {
                        "type": "string",
                        "description" : "Zone number."	
                    },
                    "categoryAttributeId": {
                        "$ref": "#/AttributeId",
                        "description" : "The identifier of the zone category attribute."	
                    },
                    "stampPosition": {
                        "$ref": "#/Coordinate2D",
                        "description" : "Position of the origin of the zone stamp."
                    },
                    "geometry": {
                        "$ref": "#/ZoneCreationGeometry"
                    }
                },
                "additionalProperties": false,
                "required": [
                    "name",
                    "numberStr",
                    "geometry"
                ]
            }
        }
    },
    "additionalProperties": false,
    "required": [
        "zonesData"
    ]
})";
}

GS::Optional<GS::ObjectState> CreateZonesCommand::SetTypeSpecificParameters (API_Element& element, API_ElementMemo& memo, const Stories& /*stories*/, const GS::ObjectState& parameters) const
{
    parameters.Get ("floorIndex", element.header.floorInd);

    const API_Guid categoryAttrGuid = GetGuidFromArrayItem ("categoryAttributeId", parameters);
    if (categoryAttrGuid != APINULLGuid) {
        element.zone.catInd = GetAttributeIndexFromGuid (API_ZoneCatID, categoryAttrGuid);
    }

    if (!SetUCharProperty (&parameters, "name", element.zone.roomName)) {
        return CreateErrorResponse (APIERR_BADPARS, "Invalid or missing name parameter.");
    }

    if (!SetUCharProperty (&parameters, "numberStr", element.zone.roomNoStr)) {
        return CreateErrorResponse (APIERR_BADPARS, "Invalid or missing numberStr parameter.");
    }

    GS::ObjectState geometry;
    if (!parameters.Get ("geometry", geometry)) {
        return CreateErrorResponse (APIERR_BADPARS, "geometry parameter is missing.");
    }

    GS::ObjectState stampPosition;
    parameters.Get ("stampPosition", stampPosition);

    GS::ObjectState referencePosition;
    if (geometry.Get ("referencePosition", referencePosition)) {
        element.zone.manual = false;

        element.zone.refPos = Get2DCoordinateFromObjectState (referencePosition);

        element.zone.pos = stampPosition.IsEmpty() ? element.zone.refPos : Get2DCoordinateFromObjectState (stampPosition);
    } else {
        element.zone.manual = true;

        GS::Array<GS::ObjectState> polygonCoordinates;
        GS::Array<GS::ObjectState> polygonArcs;
        GS::Array<GS::ObjectState> holes;
        if (!geometry.Get ("polygonCoordinates", polygonCoordinates)) {
            return CreateErrorResponse (APIERR_BADPARS, "polygonCoordinates parameter is missing in geometry.");
        }

        geometry.Get ("polygonArcs", polygonArcs);
        geometry.Get ("holes", holes);
        if (IsSame2DCoordinate (polygonCoordinates.GetFirst (), polygonCoordinates.GetLast ())) {
            polygonCoordinates.Pop ();
        }
        element.zone.poly.nCoords	= polygonCoordinates.GetSize() + 1;
        element.zone.poly.nSubPolys	= 1;
        element.zone.poly.nArcs		= polygonArcs.GetSize ();

        for (const GS::ObjectState& hole : holes) {
            GS::Array<GS::ObjectState> holePolygonOutline;
            GS::Array<GS::ObjectState> holePolygonArcs;
            if (GetHoleGeometry (hole, holePolygonOutline, holePolygonArcs)) {
                element.zone.poly.nCoords += holePolygonOutline.GetSize () + 1;
                ++element.zone.poly.nSubPolys;
                element.zone.poly.nArcs += holePolygonArcs.GetSize ();
            }
        }

        memo.coords = reinterpret_cast<API_Coord**> (BMAllocateHandle ((element.zone.poly.nCoords + 1) * sizeof (API_Coord), ALLOCATE_CLEAR, 0));
        memo.pends = reinterpret_cast<Int32**> (BMAllocateHandle ((element.zone.poly.nSubPolys + 1) * sizeof (Int32), ALLOCATE_CLEAR, 0));
        memo.parcs = reinterpret_cast<API_PolyArc**> (BMAllocateHandle (element.zone.poly.nArcs * sizeof (API_PolyArc), ALLOCATE_CLEAR, 0));

        Int32 iCoord = 1;
        Int32 iArc = 0;
        Int32 iPends = 1;
        AddPolyToMemo(polygonCoordinates,
                      polygonArcs,
                      iCoord,
                      iArc,
                      iPends,
                      memo);

        for (const GS::ObjectState& hole : holes) {
            GS::Array<GS::ObjectState> holePolygonOutline;
            GS::Array<GS::ObjectState> holePolygonArcs;
            if (GetHoleGeometry (hole, holePolygonOutline, holePolygonArcs)) {
                AddPolyToMemo (holePolygonOutline,
                              holePolygonArcs,
                              iCoord,
                              iArc,
                              iPends,
                              memo);
            }
        }

        element.zone.pos = stampPosition.IsEmpty() ? (*memo.coords)[1] : Get2DCoordinateFromObjectState (stampPosition);
    }

    return {};
}

CreatePolylinesCommand::CreatePolylinesCommand () :
    CreateElementsCommandBase ("CreatePolylines", API_PolyLineID, "polylinesData")
{
}

GS::Optional<GS::UniString> CreatePolylinesCommand::GetInputParametersSchema () const
{
    return R"({
    "type": "object",
    "properties": {
        "polylinesData": {
            "type": "array",
            "description": "Array of data to create Polylines.",
            "items": {
                "type": "object",
                "description" : "The parameters of the new Polyline.",
                "properties" : {
                    "floorInd": {
                        "type": "number",
                        "description" : "The identifier of the floor. Optional parameter, by default the current floor is used."	
                    },
                    "coordinates": { 
                        "type": "array",
                        "description": "The 2D coordinates of the polyline.",
                        "items": {
                            "$ref": "#/Coordinate2D"
                        },
                        "minItems": 2
                    },
                    "arcs": { 
                        "type": "array",
                        "description": "The arcs of the polyline.",
                        "items": {
                            "$ref": "#/PolyArc"
                        }
                    }
                },
                "additionalProperties": false,
                "required" : [
                    "coordinates"
                ]
            }
        }
    },
    "additionalProperties": false,
    "required": [
        "polylinesData"
    ]
})";
}

static void AddPolyToMemo (const GS::Array<GS::ObjectState>& coordinates,
                           const GS::Array<GS::ObjectState>& arcs,
                           API_Polygon&                      poly,
                           API_ElementMemo& 				 memo)
{
    const GS::Array<API_PolyArc> polyArcs = GetPolyArcs (arcs, 1);
    poly.nCoords	= coordinates.GetSize();
    poly.nSubPolys	= 1;
    poly.nArcs		= polyArcs.GetSize ();

    memo.coords = reinterpret_cast<API_Coord**> (BMAllocateHandle ((poly.nCoords + 1) * sizeof (API_Coord), ALLOCATE_CLEAR, 0));
    memo.pends = reinterpret_cast<Int32**> (BMAllocateHandle ((poly.nSubPolys + 1) * sizeof (Int32), ALLOCATE_CLEAR, 0));
    memo.parcs = reinterpret_cast<API_PolyArc**> (BMAllocateHandle (poly.nArcs * sizeof (API_PolyArc), ALLOCATE_CLEAR, 0));

    Int32 iCoord = 0;
    for (const GS::ObjectState& c : coordinates) {
        (*memo.coords)[++iCoord] = Get2DCoordinateFromObjectState (c);
    }
    (*memo.pends)[1] = iCoord;

    Int32 iArc = 0;
    for (const API_PolyArc& a : polyArcs) {
        (*memo.parcs)[iArc++] = a;
    }
}

GS::Optional<GS::ObjectState> CreatePolylinesCommand::SetTypeSpecificParameters (API_Element& element, API_ElementMemo& memo, const Stories&, const GS::ObjectState& parameters) const
{
    parameters.Get ("floorInd", element.header.floorInd);

    GS::Array<GS::ObjectState> coordinates;
    GS::Array<GS::ObjectState> arcs;
    parameters.Get ("coordinates", coordinates);
    parameters.Get ("arcs", arcs);

    AddPolyToMemo(coordinates,
                  arcs,
                  element.polyLine.poly,
                  memo);

    return {};
}

CreateObjectsCommand::CreateObjectsCommand () :
    CreateElementsCommandBase ("CreateObjects", API_ObjectID, "objectsData")
{
}

GS::Optional<GS::UniString> CreateObjectsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "objectsData": {
                "type": "array",
                "description": "Array of data to create Objects.",
                "items": {
                    "type": "object",
                    "description": "The parameters of the new Object.",
                    "properties": {
                        "libraryPartName": {
                            "type": "string",
                            "description" : "The name of the library part to use."	
                        },
                        "coordinates": {
                            "$ref": "#/Coordinate3D"
                        },
                        "dimensions": {
                            "$ref": "#/Dimensions3D"
                        }
                    },
                    "additionalProperties": false,
                    "required" : [
                        "libraryPartName",
                        "coordinates"
                    ]
                }
            }
        },
        "additionalProperties": false,
        "required": [
            "objectsData"
        ]
    })";
}

constexpr const char* ParameterValueFieldName = "value";

static void SetParamValueInteger (API_AddParType&        addPar,
					              const GS::ObjectState& parameterDetails)
{
	Int32 value;
	parameterDetails.Get (ParameterValueFieldName, value);
	addPar.value.real = value;
}

static void SetParamValueDouble (API_AddParType&        addPar,
					             const GS::ObjectState&	parameterDetails)
{
	double value;
	parameterDetails.Get (ParameterValueFieldName, value);
	addPar.value.real = value;
}

static void SetParamValueOnOff (API_AddParType&         addPar,
				                const GS::ObjectState&	parameterDetails)
{
	GS::String value;
	parameterDetails.Get (ParameterValueFieldName, value);
	addPar.value.real = (value == "Off" ? 0 : 1);
}

static void SetParamValueBool (API_AddParType&        addPar,
				               const GS::ObjectState& parameterDetails)
{
	bool value;
	parameterDetails.Get (ParameterValueFieldName, value);
	addPar.value.real = (value ? 0 : 1);
}

static void SetParamValueString (API_AddParType&        addPar,
					             const GS::ObjectState&	parameterDetails)
{
	GS::UniString value;
	parameterDetails.Get (ParameterValueFieldName, value);

	GS::ucscpy (addPar.value.uStr, value.ToUStr (0, GS::Min(value.GetLength (), (USize)API_UAddParStrLen)).Get ());
}

static void ChangeParams (API_AddParType**& params, const GS::HashTable<GS::String, GS::ObjectState>& changeParamsDictionary)
{
	const GSSize nParams = BMGetHandleSize ((GSHandle) params) / sizeof (API_AddParType);
	for (GSIndex ii = 0; ii < nParams; ++ii) {
		API_AddParType& actParam = (*params)[ii];

		const GS::String name(actParam.name);
		const auto* value = changeParamsDictionary.GetPtr (name);
		if (value == nullptr)
			continue;

		switch (actParam.typeID) {
			case APIParT_Integer:
			case APIParT_PenCol:			SetParamValueInteger (actParam, *value); break;
			case APIParT_ColRGB:
			case APIParT_Intens:
			case APIParT_Length:
			case APIParT_RealNum:
			case APIParT_Angle:				SetParamValueDouble (actParam, *value);	 break;
			case APIParT_LightSw:			SetParamValueOnOff (actParam, *value); 	 break;
			case APIParT_Boolean: 			SetParamValueBool (actParam, *value);	 break;
			case APIParT_LineTyp:
			case APIParT_Mater:
			case APIParT_FillPat:
			case APIParT_BuildingMaterial:
			case APIParT_Profile: 			SetParamValueInteger (actParam, *value); break;
			case APIParT_CString:
			case APIParT_Title: 			SetParamValueString (actParam, *value);	 break;
			default:
			case APIParT_Dictionary:
				// Not supported by the Archicad API yet
				break;
		}
	}
}

GS::Optional<GS::ObjectState> CreateObjectsCommand::SetTypeSpecificParameters (API_Element& element, API_ElementMemo& memo, const Stories& stories, const GS::ObjectState& parameters) const
{
    GS::UniString uName;
    parameters.Get ("libraryPartName", uName);

    API_LibPart libPart = {};
    GS::ucscpy (libPart.docu_UName, uName.ToUStr ());

    GSErrCode err = ACAPI_LibraryPart_Search (&libPart, false, true);
    delete libPart.location;

    if (err != NoError) {
        return CreateErrorResponse (err, GS::UniString::Printf ("Not found library part with name '%T'", uName.ToPrintf()));
    }

    element.object.libInd = libPart.index;

    GS::ObjectState coordinates;
    if (parameters.Get ("coordinates", coordinates)) {
        const API_Coord3D apiCoordinate = Get3DCoordinateFromObjectState (coordinates);

        element.object.pos.x = apiCoordinate.x;
        element.object.pos.y = apiCoordinate.y;

        const auto floorIndexAndOffset = GetFloorIndexAndOffset (apiCoordinate.z, stories);
        element.header.floorInd = floorIndexAndOffset.first;
        element.object.level = floorIndexAndOffset.second;
    }

    if (parameters.Get ("dimensions", coordinates)) {
        const API_Coord3D dimensions = Get3DCoordinateFromObjectState (coordinates);

        element.object.xRatio = dimensions.x;
        element.object.yRatio = dimensions.y;
        GS::ObjectState os (ParameterValueFieldName, dimensions.z);
        ChangeParams(memo.params, {{"ZZYZX", os}});
    }

    return {};
}

CreateMeshesCommand::CreateMeshesCommand () :
    CreateElementsCommandBase ("CreateMeshes", API_MeshID, "meshesData")
{}

GS::Optional<GS::UniString> CreateMeshesCommand::GetInputParametersSchema () const
{
    return R"({
    "type": "object",
    "properties": {
        "meshesData": {
            "type": "array",
            "description": "Array of data to create Meshes.",
            "items": {
                "type": "object",
                "description" : "The parameters of the new Mesh.",
                "properties" : {
                    "floorIndex": {
                        "type": "integer"
                    },
                    "level": {
                        "type": "number",
                        "description": "The Z reference level of coordinates."
                    },
                    "skirtType": {
                        "$ref": "#/MeshSkirtType"
                    },
                    "skirtLevel": {
                        "type": "number",
                        "description": "The height of the skirt."
                    },
                    "polygonCoordinates": { 
                        "type": "array",
                        "description": "The 3D coordinates of the outline polygon of the mesh.",
                        "items": {
                            "$ref": "#/Coordinate3D"
                        },
                        "minItems": 3
                    },
                    "polygonArcs": {
                        "type": "array",
                        "description": "Polygon outline arcs of the mesh.",
                        "items": {
                            "$ref": "#/PolyArc"
                        }
                    },
                    "holes" : {
                        "$ref": "#/Holes3D"
                    },
                    "sublines": {
                        "type": "array",
                        "description": "The leveling sublines inside the polygon of the mesh.",
                        "items": {
                            "type": "object",
                            "properties" : {
                                "coordinates": { 
                                    "type": "array",
                                    "description": "The 3D coordinates of the leveling subline of the mesh.",
                                    "items": {
                                        "$ref": "#/Coordinate3D"
                                    }
                                }
                            },
                            "additionalProperties": false,
                            "required": [
                                "coordinates"
                            ]
                        },
                        "minItems": 1
                    }
                },
                "additionalProperties": false,
                "required": [
                    "polygonCoordinates"
                ]
            }
        }
    },
    "additionalProperties": false,
    "required": [
        "meshesData"
    ]
})";
}

GS::Optional<GS::ObjectState> CreateMeshesCommand::SetTypeSpecificParameters (API_Element& element, API_ElementMemo& memo, const Stories& /*stories*/, const GS::ObjectState& parameters) const
{
    parameters.Get ("floorIndex", element.header.floorInd);
    parameters.Get ("level", element.mesh.level);
    parameters.Get ("skirtLevel", element.mesh.skirtLevel);
    GS::UniString skirtType;
    parameters.Get ("skirtType", skirtType);
    if (skirtType == "SurfaceOnlyWithoutSkirt") {
        element.mesh.skirt = 3;
    } else if (skirtType == "WithSkirt") {
        element.mesh.skirt = 2;
    } else if (skirtType == "SolidBodyWithSkirt") {
        element.mesh.skirt = 1;
    }

    GS::Array<GS::ObjectState> polygonCoordinates;
    GS::Array<GS::ObjectState> polygonArcs;
    GS::Array<GS::ObjectState> holes;
    parameters.Get ("polygonCoordinates", polygonCoordinates);
    parameters.Get ("polygonArcs", polygonArcs);
    parameters.Get ("holes", holes);
    if (IsSame2DCoordinate (polygonCoordinates.GetFirst (), polygonCoordinates.GetLast ())) {
        polygonCoordinates.Pop ();
    }
    element.mesh.poly.nCoords = polygonCoordinates.GetSize () + 1;
    element.mesh.poly.nSubPolys = 1;
    element.mesh.poly.nArcs = polygonArcs.GetSize ();

    for (const GS::ObjectState& hole : holes) {
        GS::Array<GS::ObjectState> holePolygonOutline;
        GS::Array<GS::ObjectState> holePolygonArcs;
        if (GetHoleGeometry (hole, holePolygonOutline, holePolygonArcs)) {
            element.mesh.poly.nCoords += holePolygonOutline.GetSize () + 1;
            ++element.mesh.poly.nSubPolys;
            element.mesh.poly.nArcs += holePolygonArcs.GetSize ();
        }
    }

    memo.coords = reinterpret_cast<API_Coord**> (BMAllocateHandle ((element.mesh.poly.nCoords + 1) * sizeof (API_Coord), ALLOCATE_CLEAR, 0));
    memo.meshPolyZ = reinterpret_cast<double**> (BMAllocateHandle ((element.mesh.poly.nCoords + 1) * sizeof (double), ALLOCATE_CLEAR, 0));
    memo.pends = reinterpret_cast<Int32**> (BMAllocateHandle ((element.mesh.poly.nSubPolys + 1) * sizeof (Int32), ALLOCATE_CLEAR, 0));
    memo.parcs = reinterpret_cast<API_PolyArc**> (BMAllocateHandle (element.mesh.poly.nArcs * sizeof (API_PolyArc), ALLOCATE_CLEAR, 0));

    Int32 iCoord = 1;
    Int32 iArc = 0;
    Int32 iPends = 1;
    AddPolyToMemo (polygonCoordinates,
                   polygonArcs,
                   iCoord,
                   iArc,
                   iPends,
                   memo);

    for (const GS::ObjectState& hole : holes) {
        GS::Array<GS::ObjectState> holePolygonOutline;
        GS::Array<GS::ObjectState> holePolygonArcs;
        if (GetHoleGeometry (hole, holePolygonOutline, holePolygonArcs)) {
            AddPolyToMemo (holePolygonOutline,
                           holePolygonArcs,
                           iCoord,
                           iArc,
                           iPends,
                           memo);
        }
    }

    GS::Array<GS::ObjectState> sublines;
    parameters.Get ("sublines", sublines);
    element.mesh.levelLines.nSubLines = 0;
    element.mesh.levelLines.nCoords = 0;
    for (const GS::ObjectState& subline : sublines) {
        GS::Array<GS::ObjectState> coordinates;
        subline.Get ("coordinates", coordinates);
        if (coordinates.IsEmpty ()) {
            continue;
        }

        ++element.mesh.levelLines.nSubLines;
        element.mesh.levelLines.nCoords += coordinates.GetSize ();
    }

    memo.meshLevelCoords = reinterpret_cast<API_MeshLevelCoord**> (BMAllocateHandle (element.mesh.levelLines.nCoords * sizeof (API_MeshLevelCoord), ALLOCATE_CLEAR, 0));
    memo.meshLevelEnds = reinterpret_cast<Int32**> (BMAllocateHandle (element.mesh.levelLines.nSubLines * sizeof (Int32), ALLOCATE_CLEAR, 0));

    Int32 vertexID = 0;
    Int32 lineID = 0;
    for (const GS::ObjectState& subline : sublines) {
        GS::Array<GS::ObjectState> coordinates;
        subline.Get ("coordinates", coordinates);
        if (coordinates.IsEmpty ()) {
            continue;
        }

        for (const GS::ObjectState& coord : coordinates) {
            API_MeshLevelCoord& meshLevelCoord = (*memo.meshLevelCoords)[vertexID];
            meshLevelCoord.c = Get3DCoordinateFromObjectState (coord);
            meshLevelCoord.vertexID = vertexID++;
        }
        (*memo.meshLevelEnds)[lineID++] = vertexID;
    }

    return {};
}

CreateLabelsCommand::CreateLabelsCommand () :
    CreateElementsCommandBase ("CreateLabels", API_LabelID, "labelsData")
{
}

GS::Optional<GS::UniString> CreateLabelsCommand::GetInputParametersSchema () const
{
    return R"({
    "type": "object",
    "properties": {
        "labelsData": {
            "type": "array",
            "description": "Array of data to create Labels.",
            "items": {
                "type": "object",
                "description": "The parameters of the new Label.",
                "properties": {
                    "parentElementId": {
                        "$ref": "#/ElementId",
                        "description" : "The parent element if the label is an associative label."	
                    },
                    "text": { 
                        "type": "string",
                        "description": "The text content if the label is a text label."
                    },
                    "begCoordinate": {
                        "$ref": "#/Coordinate2D",
                        "description": "The begin coordinate of leader line. Optional parameter, but either begCoordinate or parentElementId must be provided."
                    },
                    "floorInd": {
                        "type": "number",
                        "description" : "The identifier of the floor. Optional parameter, by default the current floor or the floor of the parent element is used."	
                    }
                },
                "additionalProperties": false,
                "required": [
                ]
            }
        }
    },
    "additionalProperties": false,
    "required": [
        "labelsData"
    ]
})";
}

static GSErrCode SetParagraph (API_ParagraphType** paragraph, UInt32 parNum, Int32 from, Int32 range, Int32 numOfTabs, Int32 numOfRuns,
							   Int32 numOfeolPos)
{
	if (paragraph == nullptr || parNum >= (BMhGetSize (reinterpret_cast<GSHandle> (paragraph)) / sizeof (API_ParagraphType)))
		return APIERR_BADPARS;

	if (numOfTabs < 1 || numOfRuns < 1 || numOfeolPos < 0)
		return APIERR_BADPARS;

	(*paragraph)[parNum].from = from;
	(*paragraph)[parNum].range = range;

	(*paragraph)[parNum].tab = reinterpret_cast<API_TabType*> (BMpAllClear (numOfTabs * sizeof (API_TabType)));
	(*paragraph)[parNum].run = reinterpret_cast<API_RunType*> (BMpAllClear (numOfRuns * sizeof (API_RunType)));

	if (numOfeolPos > 0) {
		(*paragraph)[parNum].eolPos = reinterpret_cast<Int32*> (BMpAllClear (numOfeolPos * sizeof (Int32)));
	}

	return NoError;
}

static GSErrCode SetRun (API_ParagraphType** paragraph, UInt32 parNum, UInt32 runNum, Int32 from, Int32 range, short pen, unsigned short faceBits,
						 short font, Int32 effectBits, double size)
{
	if (paragraph == nullptr || parNum >= (BMhGetSize (reinterpret_cast<GSHandle> (paragraph)) / sizeof (API_ParagraphType)))
		return APIERR_BADPARS;

	if (runNum >= BMGetPtrSize (reinterpret_cast<GSPtr> ((*paragraph)[parNum].run)) / sizeof (API_RunType))
		return APIERR_BADPARS;

	(*paragraph)[parNum].run[runNum].from	    = from;
	(*paragraph)[parNum].run[runNum].range	    = range;
	(*paragraph)[parNum].run[runNum].pen	    = pen;
	(*paragraph)[parNum].run[runNum].faceBits   = faceBits;
	(*paragraph)[parNum].run[runNum].font	    = font;
	(*paragraph)[parNum].run[runNum].effectBits = (unsigned short)effectBits;
	(*paragraph)[parNum].run[runNum].size	    = size;

	return NoError;
}

static GSErrCode SetEOL (API_ParagraphType** paragraph, UInt32 parNum, UInt32 eolNum, Int32 offset)
{
	if (paragraph == nullptr || parNum >= (BMhGetSize (reinterpret_cast<GSHandle> (paragraph)) / sizeof (API_ParagraphType)))
		return APIERR_BADPARS;

	if (eolNum >= BMGetPtrSize (reinterpret_cast<GSPtr> ((*paragraph)[parNum].eolPos)) / sizeof (Int32))
		return APIERR_BADPARS;

	if (offset < 0)
		return APIERR_BADPARS;

	(*paragraph)[parNum].eolPos[eolNum] = offset;

	return NoError;
}

GS::Optional<GS::ObjectState> CreateLabelsCommand::SetTypeSpecificParameters (API_Element& element, API_ElementMemo& memo, const Stories&, const GS::ObjectState& parameters) const
{
    parameters.Get ("floorInd", element.header.floorInd);

    const GS::ObjectState* begCOS = parameters.Get ("begCoordinate");

    element.label.parent = GetGuidFromArrayItem ("parentElementId", parameters);
    API_Elem_Head parentElemHead = {};
    if (element.label.parent != APINULLGuid) {
        parentElemHead.guid = element.label.parent;
        if (ACAPI_Element_GetHeader (&parentElemHead) == NoError) {
#ifdef ServerMainVers_2600
            element.label.parentType = parentElemHead.type;
#else
            element.label.parentType = parentElemHead.typeID;
#endif
        } else {
            return CreateErrorResponse (APIERR_BADPARS, "Invalid parent element GUID");
        }

        element.header.floorInd = parentElemHead.floorInd;
    }

    if (begCOS != nullptr) {
        element.label.begC = Get2DCoordinateFromObjectState (*begCOS);
    } else if (parentElemHead.guid != APINULLGuid) {
        API_Box3D box = {};
        ACAPI_Element_CalcBounds (&parentElemHead, &box);
        element.label.begC.x = (box.xMin + box.xMax) / 2.0;
        element.label.begC.y = (box.yMin + box.yMax) / 2.0;
    } else {
        return CreateErrorResponse (APIERR_BADPARS, "Missing 'begCoordinate' parameter");
    }
    element.label.createAtDefaultPosition = true;

    if (element.label.labelClass == APILblClass_Text) {
        GS::UniString text;
        if (!parameters.Get ("text", text)) {
            return CreateErrorResponse (APIERR_BADPARS, "Missing 'text' parameter for text label");
        }
#ifdef ServerMainVers_2800
        delete memo.textContent;
        memo.textContent = new GS::UniString { text };
#else
        memo.textContent = BMhAllClear ((text.GetLength () + 1) * sizeof (GS::uchar_t));
        GS::ucscpy (reinterpret_cast<GS::uchar_t*> (*memo.textContent), text.ToUStr ());
#endif

        const GS::UniChar newlineChar = GS::UniChar (char ('\n'));
        element.label.u.text.nLine = text.Count(newlineChar) + 1;
	    const Int32 numOfParagraphs = 1;
	    memo.paragraphs = reinterpret_cast<API_ParagraphType**> (BMhAll (numOfParagraphs * sizeof (API_ParagraphType)));
        SetParagraph (memo.paragraphs, 0, 0, text.GetLength (), 1, 1, element.label.u.text.nLine);
        SetRun (memo.paragraphs, 0, 0, 0, text.GetLength (), element.label.u.text.pen, element.label.u.text.faceBits, element.label.u.text.font, element.label.u.text.effectsBits, element.label.u.text.size);
        Int32 lastEolPos = 0;
        for (Int32 eolIndex = 0; eolIndex < element.label.u.text.nLine; ++eolIndex) {
            Int32 eolPos = text.FindFirst (newlineChar, eolIndex == 0 ? 0 : lastEolPos + 1);
            Int32 offset = (eolPos != MaxUIndex ? eolPos : text.GetLength ()) - lastEolPos - 1;
            lastEolPos = eolPos;
            SetEOL (memo.paragraphs, 0, eolIndex, offset);
        }

        element.label.u.text.width  = 0;
        element.label.u.text.height = 0;
        element.label.u.text.nonBreaking = true;
        element.label.u.text.useEolPos = true;
    }

    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// CreateWallsCommand
// ─────────────────────────────────────────────────────────────────────────────

CreateWallsCommand::CreateWallsCommand () :
    CreateElementsCommandBase ("CreateWalls", API_WallID, "wallsData")
{
}

GS::Optional<GS::UniString> CreateWallsCommand::GetInputParametersSchema () const
{
    return R"({
    "type": "object",
    "properties": {
        "wallsData": {
            "type": "array",
            "description": "Array of data to create Walls.",
            "items": {
                "type": "object",
                "description": "The parameters of the new Wall.",
                "properties": {
                    "begCoordinate": {
                        "$ref": "#/Coordinate2D",
                        "description": "The start point of the wall reference line."
                    },
                    "endCoordinate": {
                        "$ref": "#/Coordinate2D",
                        "description": "The end point of the wall reference line."
                    },
                    "height": {
                        "type": "number",
                        "description": "The height of the wall in metres."
                    },
                    "bottomOffset": {
                        "type": "number",
                        "description": "Vertical offset of the wall base from the floor level. Default: 0."
                    },
                    "offset": {
                        "type": "number",
                        "description": "Lateral offset of the wall body from the reference line. Default: 0."
                    },
                    "thickness": {
                        "type": "number",
                        "description": "Wall thickness in metres (used only when no composite or building material is provided)."
                    },
                    "floorIndex": {
                        "type": "integer",
                        "description": "Story (floor) index. Optional, uses the current story if omitted."
                    },
                    "layerAttributeId": {
                        "$ref": "#/AttributeId",
                        "description": "The identifier of the layer attribute."
                    },
                    "compositeAttributeId": {
                        "$ref": "#/AttributeId",
                        "description": "The identifier of the composite wall attribute. Takes precedence over buildingMaterialAttributeId."
                    },
                    "buildingMaterialAttributeId": {
                        "$ref": "#/AttributeId",
                        "description": "The identifier of the building material attribute (for single-skin walls)."
                    }
                },
                "additionalProperties": false,
                "required": [
                    "begCoordinate",
                    "endCoordinate",
                    "height"
                ]
            }
        }
    },
    "additionalProperties": false,
    "required": [
        "wallsData"
    ]
})";
}

GS::Optional<GS::ObjectState> CreateWallsCommand::SetTypeSpecificParameters (
    API_Element& element, API_ElementMemo& /*memo*/, const Stories& /*stories*/, const GS::ObjectState& parameters) const
{
    GS::ObjectState begCoordinate;
    GS::ObjectState endCoordinate;
    if (!parameters.Get ("begCoordinate", begCoordinate) ||
        !parameters.Get ("endCoordinate",  endCoordinate)) {
        return CreateErrorResponse (APIERR_BADPARS, "Missing begCoordinate or endCoordinate.");
    }
    element.wall.begC = Get2DCoordinateFromObjectState (begCoordinate);
    element.wall.endC = Get2DCoordinateFromObjectState (endCoordinate);

    if (!parameters.Get ("height", element.wall.height)) {
        return CreateErrorResponse (APIERR_BADPARS, "Missing height parameter.");
    }

    parameters.Get ("bottomOffset", element.wall.bottomOffset);
    parameters.Get ("offset",       element.wall.offset);

    parameters.Get ("floorIndex", element.header.floorInd);

    const API_Guid layerGuid = GetGuidFromArrayItem ("layerAttributeId", parameters);
    if (layerGuid != APINULLGuid) {
        element.header.layer = GetAttributeIndexFromGuid (API_LayerID, layerGuid);
    }

    const API_Guid compositeGuid = GetGuidFromArrayItem ("compositeAttributeId", parameters);
    if (compositeGuid != APINULLGuid) {
        element.wall.composite              = GetAttributeIndexFromGuid (API_CompWallID, compositeGuid);
        element.wall.modelElemStructureType = APIModelElemStructure_Composite;
    } else {
        const API_Guid bmGuid = GetGuidFromArrayItem ("buildingMaterialAttributeId", parameters);
        if (bmGuid != APINULLGuid) {
            element.wall.buildingMaterial       = GetAttributeIndexFromGuid (API_BuildingMaterialID, bmGuid);
            element.wall.modelElemStructureType = APIModelElemStructure_Basic;
        } else {
            double thickness = 0.25;
            parameters.Get ("thickness", thickness);
            element.wall.thickness              = thickness;
            element.wall.modelElemStructureType = APIModelElemStructure_Basic;
        }
    }

    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// Shared helper for CreateDoors / CreateWindows
// ─────────────────────────────────────────────────────────────────────────────

static GS::Optional<GS::ObjectState> SetOpeningParameters (
    API_Element& element, const GS::ObjectState& parameters)
{
    // Host wall (required)
    const API_Guid ownerGuid = GetGuidFromArrayItem ("hostWallId", parameters);
    if (ownerGuid == APINULLGuid) {
        return CreateErrorResponse (APIERR_BADPARS, "Missing or invalid hostWallId.");
    }
    element.window.owner = ownerGuid;

    // Inherit floor from host wall
    API_Elem_Head wallHead = {};
    wallHead.guid = ownerGuid;
    if (ACAPI_Element_GetHeader (&wallHead) == NoError) {
        element.header.floorInd = wallHead.floorInd;
    }

    // Library part (optional – keep whatever GetDefaults supplied if omitted)
    GS::UniString libPartName;
    if (parameters.Get ("libraryPartName", libPartName) && !libPartName.IsEmpty ()) {
        API_LibPart libPart = {};
        GS::ucscpy (libPart.docu_UName, libPartName.ToUStr ());
        GSErrCode err = ACAPI_LibraryPart_Search (&libPart, false, true);
        delete libPart.location;
        if (err != NoError) {
            return CreateErrorResponse (err,
                GS::UniString::Printf ("Library part not found: '%T'", libPartName.ToPrintf ()));
        }
        element.window.openingBase.libInd = libPart.index;
    }

    // Dimensions
    double width = 0.0, height = 0.0;
    if (parameters.Get ("openingWidth",  width)  && width  > 0.0) element.window.openingBase.width  = width;
    if (parameters.Get ("openingHeight", height) && height > 0.0) element.window.openingBase.height = height;

    // Position along wall from wall start (centre of the opening)
    double position = 0.0;
    if (parameters.Get ("position", position)) {
        element.window.openingBase.objLoc = position;
    }

    // Sill height (floor-to-sill distance)
    double sillHeight = 0.0;
    if (parameters.Get ("sillHeight", sillHeight)) {
        element.window.openingBase.subFloorThickness = sillHeight;
    }

    // Flip flags
    bool flipX = false, flipY = false;
    parameters.Get ("flipX", flipX);
    parameters.Get ("flipY", flipY);
    if (flipX) element.window.reflected = !element.window.reflected;
    if (flipY) element.window.oSide     = !element.window.oSide;

    // Layer
    const API_Guid layerGuid = GetGuidFromArrayItem ("layerAttributeId", parameters);
    if (layerGuid != APINULLGuid) {
        element.header.layer = GetAttributeIndexFromGuid (API_LayerID, layerGuid);
    }

    return {};
}

static GS::UniString GetOpeningSchema (const GS::String& arrayFieldName, const GS::String& description)
{
    return GS::UniString::Printf (R"({
    "type": "object",
    "properties": {
        "%s": {
            "type": "array",
            "description": "%s",
            "items": {
                "type": "object",
                "description": "Parameters of a single opening.",
                "properties": {
                    "hostWallId": {
                        "$ref": "#/ElementId",
                        "description": "GUID of the host wall."
                    },
                    "libraryPartName": {
                        "type": "string",
                        "description": "Name of the library part. Uses the current default if omitted."
                    },
                    "position": {
                        "type": "number",
                        "description": "Distance from wall start to the centre of the opening, in metres."
                    },
                    "sillHeight": {
                        "type": "number",
                        "description": "Sill height (floor to sill bottom) in metres. Default: 0."
                    },
                    "openingWidth": {
                        "type": "number",
                        "description": "Opening width in metres."
                    },
                    "openingHeight": {
                        "type": "number",
                        "description": "Opening height in metres."
                    },
                    "flipX": {
                        "type": "boolean",
                        "description": "Flip the opening horizontally. Default: false."
                    },
                    "flipY": {
                        "type": "boolean",
                        "description": "Flip the opening to the other side of the wall. Default: false."
                    },
                    "layerAttributeId": {
                        "$ref": "#/AttributeId",
                        "description": "The identifier of the layer attribute."
                    }
                },
                "additionalProperties": false,
                "required": [
                    "hostWallId"
                ]
            }
        }
    },
    "additionalProperties": false,
    "required": [
        "%s"
    ]
})", arrayFieldName.ToPrintf (), description.ToPrintf (), arrayFieldName.ToPrintf ());
}

// ─────────────────────────────────────────────────────────────────────────────
// CreateDoorsCommand
// ─────────────────────────────────────────────────────────────────────────────

CreateDoorsCommand::CreateDoorsCommand () :
    CreateElementsCommandBase ("CreateDoors", API_DoorID, "doorsData")
{
}

GS::Optional<GS::UniString> CreateDoorsCommand::GetInputParametersSchema () const
{
    return GetOpeningSchema ("doorsData", "Array of data to create Doors.");
}

GS::Optional<GS::ObjectState> CreateDoorsCommand::SetTypeSpecificParameters (
    API_Element& element, API_ElementMemo& /*memo*/, const Stories& /*stories*/, const GS::ObjectState& parameters) const
{
    return SetOpeningParameters (element, parameters);
}

// ─────────────────────────────────────────────────────────────────────────────
// CreateWindowsCommand
// ─────────────────────────────────────────────────────────────────────────────

CreateWindowsCommand::CreateWindowsCommand () :
    CreateElementsCommandBase ("CreateWindows", API_WindowID, "windowsData")
{
}

GS::Optional<GS::UniString> CreateWindowsCommand::GetInputParametersSchema () const
{
    return GetOpeningSchema ("windowsData", "Array of data to create Windows.");
}

GS::Optional<GS::ObjectState> CreateWindowsCommand::SetTypeSpecificParameters (
    API_Element& element, API_ElementMemo& /*memo*/, const Stories& /*stories*/, const GS::ObjectState& parameters) const
{
    return SetOpeningParameters (element, parameters);
}

// ─────────────────────────────────────────────────────────────────────────────
// CreateRoofsCommand  (single-plane roofs only)
// ─────────────────────────────────────────────────────────────────────────────

CreateRoofsCommand::CreateRoofsCommand () :
    CreateElementsCommandBase ("CreateRoofs", API_RoofID, "roofsData")
{
}

GS::Optional<GS::UniString> CreateRoofsCommand::GetInputParametersSchema () const
{
    return R"({
    "type": "object",
    "properties": {
        "roofsData": {
            "type": "array",
            "description": "Array of data to create single-plane Roofs.",
            "items": {
                "type": "object",
                "description": "Parameters of the new Roof.",
                "properties": {
                    "outline": {
                        "type": "array",
                        "description": "2D outline polygon of the roof footprint (at least 3 points, do not repeat first point).",
                        "items": {
                            "$ref": "#/Coordinate2D"
                        },
                        "minItems": 3
                    },
                    "baseLevel": {
                        "type": "number",
                        "description": "Z coordinate of the pivot edge (metres, absolute)."
                    },
                    "pivotEdgeIndex": {
                        "type": "integer",
                        "description": "Index of the outline edge that acts as the pivot (low) edge. Default: 0."
                    },
                    "slope": {
                        "type": "number",
                        "description": "Roof slope as tan(angle). E.g. 0.20 ≈ 11.3°."
                    },
                    "thickness": {
                        "type": "number",
                        "description": "Roof thickness in metres. Default: 0.20."
                    },
                    "floorIndex": {
                        "type": "integer",
                        "description": "Story (floor) index. Optional."
                    },
                    "layerAttributeId": {
                        "$ref": "#/AttributeId",
                        "description": "The identifier of the layer attribute."
                    },
                    "compositeAttributeId": {
                        "$ref": "#/AttributeId",
                        "description": "The identifier of the composite attribute."
                    },
                    "buildingMaterialAttributeId": {
                        "$ref": "#/AttributeId",
                        "description": "The identifier of the building material attribute."
                    }
                },
                "additionalProperties": false,
                "required": [
                    "outline",
                    "baseLevel",
                    "slope"
                ]
            }
        }
    },
    "additionalProperties": false,
    "required": [
        "roofsData"
    ]
})";
}

GS::Optional<GS::ObjectState> CreateRoofsCommand::SetTypeSpecificParameters (
    API_Element& element, API_ElementMemo& memo, const Stories& /*stories*/, const GS::ObjectState& parameters) const
{
    GS::Array<GS::ObjectState> outline;
    if (!parameters.Get ("outline", outline) || outline.GetSize () < 3) {
        return CreateErrorResponse (APIERR_BADPARS, "outline must have at least 3 coordinates.");
    }

    // Remove duplicate last point if the caller closed the polygon
    if (IsSame2DCoordinate (outline.GetFirst (), outline.GetLast ())) {
        outline.Pop ();
    }

    double baseLevel = 0.0;
    parameters.Get ("baseLevel", baseLevel);
    element.roof.level = baseLevel;

    double slope = 0.20;
    parameters.Get ("slope", slope);
    element.roof.u.planeRoof.angle = std::atan (slope);
    element.roof.u.planeRoof.posSign = 1;

    Int32 pivotEdgeIndex = 0;
    parameters.Get ("pivotEdgeIndex", pivotEdgeIndex);
    const Int32 n = static_cast<Int32> (outline.GetSize ());
    pivotEdgeIndex = ((pivotEdgeIndex % n) + n) % n; // guard against out-of-range
    element.roof.u.planeRoof.baseLine.c1 = Get2DCoordinateFromObjectState (outline[pivotEdgeIndex]);
    element.roof.u.planeRoof.baseLine.c2 = Get2DCoordinateFromObjectState (outline[(pivotEdgeIndex + 1) % n]);

    element.roof.roofClass = API_PlaneRoofID;

    double thickness = 0.20;
    parameters.Get ("thickness", thickness);
    element.roof.thickness = thickness;

    parameters.Get ("floorIndex", element.header.floorInd);

    const API_Guid layerGuid = GetGuidFromArrayItem ("layerAttributeId", parameters);
    if (layerGuid != APINULLGuid) {
        element.header.layer = GetAttributeIndexFromGuid (API_LayerID, layerGuid);
    }

    const API_Guid compositeGuid = GetGuidFromArrayItem ("compositeAttributeId", parameters);
    if (compositeGuid != APINULLGuid) {
        element.roof.composite              = GetAttributeIndexFromGuid (API_CompWallID, compositeGuid);
        element.roof.modelElemStructureType = APIModelElemStructure_Composite;
    } else {
        const API_Guid bmGuid = GetGuidFromArrayItem ("buildingMaterialAttributeId", parameters);
        if (bmGuid != APINULLGuid) {
            element.roof.buildingMaterial       = GetAttributeIndexFromGuid (API_BuildingMaterialID, bmGuid);
            element.roof.modelElemStructureType = APIModelElemStructure_Basic;
        }
    }

    // Outline polygon into memo (same layout as Slab)
    element.roof.poly.nCoords   = n + 1;
    element.roof.poly.nSubPolys = 1;
    element.roof.poly.nArcs     = 0;

    memo.coords = reinterpret_cast<API_Coord**> (BMAllocateHandle ((n + 2) * sizeof (API_Coord),    ALLOCATE_CLEAR, 0));
    memo.pends  = reinterpret_cast<Int32**>     (BMAllocateHandle (2       * sizeof (Int32),         ALLOCATE_CLEAR, 0));
    memo.parcs  = reinterpret_cast<API_PolyArc**>(BMAllocateHandle (1      * sizeof (API_PolyArc),   ALLOCATE_CLEAR, 0));

    for (Int32 i = 0; i < n; ++i) {
        (*memo.coords)[i + 1] = Get2DCoordinateFromObjectState (outline[i]);
    }
    (*memo.coords)[n + 1] = (*memo.coords)[1]; // close polygon
    (*memo.pends)[1] = n + 1;

    return {};
}