"""
SQLAlchemy async engine + session factory.

`get_session` is the FastAPI dependency that yields one session per
request. MQTT handlers do NOT use it (they run outside any HTTP request);
they create sessions directly with `async with async_session() as ...`.
"""

from collections.abc import AsyncGenerator

from sqlalchemy.ext.asyncio import AsyncSession, async_sessionmaker, create_async_engine

from app.config import settings

engine = create_async_engine(settings.database_url)
async_session = async_sessionmaker(engine, class_=AsyncSession, expire_on_commit=False)


async def get_session() -> AsyncGenerator[AsyncSession, None]:
    """FastAPI dependency: yield one async DB session per HTTP request."""
    async with async_session() as session:
        yield session
