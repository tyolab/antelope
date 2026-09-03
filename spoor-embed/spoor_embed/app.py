from fastapi import FastAPI
from pydantic import BaseModel, Field
from spoor_embed.encoder import Encoder


class EmbedRequest(BaseModel):
    inputs: list[str] = Field(min_length=1)
    role: str = "doc"
    model: str | None = None


class EmbedResponse(BaseModel):
    vectors: list[list[list[float]]]
    dim: int
    model: str


def create_app(encoder: Encoder) -> FastAPI:
    app = FastAPI(title="spoor-embed", version="0.0.1")

    @app.get("/healthz")
    def healthz():
        return {"status": "ok", "model": encoder.model_name, "dim": encoder.dim}

    @app.post("/embed_multivector", response_model=EmbedResponse)
    def embed(req: EmbedRequest):
        role = "query" if req.role == "query" else "doc"
        vectors = encoder.encode(req.inputs, role=role)
        return EmbedResponse(vectors=vectors, dim=encoder.dim, model=encoder.model_name)

    return app
