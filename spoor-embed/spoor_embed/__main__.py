import os
from spoor_embed.encoder import FakeEncoder


def build_encoder():
    if os.environ.get("SPOOR_EMBED_FAKE") == "1":
        return FakeEncoder(dim=int(os.environ.get("SPOOR_EMBED_DIM", "4")))
    from spoor_embed.pylate_encoder import PyLateEncoder  # lazy: needs gpu extra
    return PyLateEncoder(model_name=os.environ.get("SPOOR_EMBED_MODEL", "lightonai/GTE-ModernColBERT-v1"))


def main():
    import uvicorn
    from spoor_embed.app import create_app
    app = create_app(build_encoder())
    uvicorn.run(app, host=os.environ.get("HOST", "0.0.0.0"), port=int(os.environ.get("PORT", "8900")))


if __name__ == "__main__":
    main()
