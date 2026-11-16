import ipaddress

from fastapi import APIRouter, Depends, File, Form, Header, HTTPException, Request, UploadFile

from app.domain import CatProfile

router = APIRouter(prefix="/api", tags=["admin"])
MAX_IMAGE_BYTES = 10 * 1024 * 1024


def require_local_action(request: Request, x_catvision_action: str = Header(default="")) -> None:
    if request.client is None or not ipaddress.ip_address(request.client.host).is_loopback:
        raise HTTPException(status_code=403, detail="administration is available on the hub computer only")
    if x_catvision_action != "local":
        raise HTTPException(status_code=403, detail="missing same-origin action header")


async def read_image(upload: UploadFile) -> bytes:
    data = await upload.read(MAX_IMAGE_BYTES + 1)
    if len(data) > MAX_IMAGE_BYTES:
        raise HTTPException(status_code=413, detail=f"{upload.filename} exceeds 10 MiB")
    return data


@router.get("/cats")
def list_cats(request: Request) -> list[CatProfile]:
    return request.app.state.cats.list()


@router.post("/cats", dependencies=[Depends(require_local_action)])
async def create_cat(
    request: Request,
    name: str = Form(min_length=1, max_length=40),
    avatar: UploadFile = File(...),
    references: list[UploadFile] = File(...),
) -> CatProfile:
    avatar_data = await read_image(avatar)
    reference_data = [await read_image(reference) for reference in references]
    try:
        return request.app.state.cats.create(name, avatar_data, reference_data)
    except ValueError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc


@router.post("/cats/{cat_id}/references", dependencies=[Depends(require_local_action)])
async def add_references(cat_id: str, request: Request, references: list[UploadFile] = File(...)) -> CatProfile:
    data = [await read_image(reference) for reference in references]
    try:
        return request.app.state.cats.add_references(cat_id, data)
    except KeyError as exc:
        raise HTTPException(status_code=404, detail="cat not found") from exc
    except ValueError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
